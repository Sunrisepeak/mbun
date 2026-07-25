// openssl.cpp — TlsChannel implementation over the ambient OpenSSL.
//
// PORT-SOURCE: see openssl.cppm. The memory-BIO drive loop matches uSockets'
// us_internal_ssl_socket handshake/data path (bun-zig-src/src/uws_sys/ssl.zig):
// SSL_do_handshake pulls from the read BIO and pushes handshake records into
// the write BIO; the caller shuttles those to/from the socket.
module;

#include <cctype>
#include <cstring>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

module mbun.tls.openssl;

import std;
import mbun.tls.tls;

namespace mbun::tls {

namespace {

std::once_flag g_sslInit;

void ensure_library() {
    std::call_once(g_sslInit, [] {
        ::OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                           nullptr);
    });
}

std::string drain_openssl_errors() {
    std::string message {};
    unsigned long code {};
    char buffer[256];
    while ((code = ::ERR_get_error()) != 0) {
        ::ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!message.empty()) {
            message += "; ";
        }
        message += buffer;
    }
    return message;
}

// node-style error code from a packed OpenSSL error: "ERR_SSL_<REASON>" for the
// SSL library, "ERR_OSSL_<REASON>" otherwise, with the reason string upcased and
// spaces turned into underscores. Mirrors node's crypto ThrowCryptoError code
// derivation (src/crypto/crypto_util.cc) so error.code matches node/bun.
std::string node_error_code(unsigned long packed) {
    const char* reason {::ERR_reason_error_string(packed)};
    if (reason == nullptr || *reason == '\0') {
        return {};
    }
    std::string code {ERR_GET_LIB(packed) == ERR_LIB_SSL ? "ERR_SSL_" : "ERR_OSSL_"};
    for (const char* p {reason}; *p != '\0'; ++p) {
        const unsigned char c {static_cast<unsigned char>(*p)};
        code.push_back(c == ' ' ? '_' : static_cast<char>(std::toupper(c)));
    }
    return code;
}

int verify_flags_for(TlsRole role, VerifyMode mode) {
    switch (mode) {
    case VerifyMode::disabled:
        return SSL_VERIFY_NONE;
    case VerifyMode::optional:
        return SSL_VERIFY_PEER;
    case VerifyMode::required:
        return role == TlsRole::server
                   ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                   : SSL_VERIFY_PEER;
    }
    return SSL_VERIFY_NONE;
}

// node src/crypto/crypto_common.cc X509ErrorCode(): a chain-verification failure
// is surfaced as the OpenSSL macro name with the X509_V_ERR_ prefix stripped
// (`UNABLE_TO_VERIFY_LEAF_SIGNATURE`, `CERT_HAS_EXPIRED`, …), which is what
// node puts in `error.code` / `socket.authorizationError`. nullptr for X509_V_OK
// and for anything not in node's table (those keep the generic handshake code).
inline const char* x509_error_code(long err) {
    switch (err) {
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: return "UNABLE_TO_GET_ISSUER_CERT";
        case X509_V_ERR_UNABLE_TO_GET_CRL: return "UNABLE_TO_GET_CRL";
        case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE: return "UNABLE_TO_DECRYPT_CERT_SIGNATURE";
        case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE: return "UNABLE_TO_DECRYPT_CRL_SIGNATURE";
        case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY: return "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";
        case X509_V_ERR_CERT_SIGNATURE_FAILURE: return "CERT_SIGNATURE_FAILURE";
        case X509_V_ERR_CRL_SIGNATURE_FAILURE: return "CRL_SIGNATURE_FAILURE";
        case X509_V_ERR_CERT_NOT_YET_VALID: return "CERT_NOT_YET_VALID";
        case X509_V_ERR_CERT_HAS_EXPIRED: return "CERT_HAS_EXPIRED";
        case X509_V_ERR_CRL_NOT_YET_VALID: return "CRL_NOT_YET_VALID";
        case X509_V_ERR_CRL_HAS_EXPIRED: return "CRL_HAS_EXPIRED";
        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD: return "ERROR_IN_CERT_NOT_BEFORE_FIELD";
        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD: return "ERROR_IN_CERT_NOT_AFTER_FIELD";
        case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD: return "ERROR_IN_CRL_LAST_UPDATE_FIELD";
        case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD: return "ERROR_IN_CRL_NEXT_UPDATE_FIELD";
        case X509_V_ERR_OUT_OF_MEM: return "OUT_OF_MEM";
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: return "DEPTH_ZERO_SELF_SIGNED_CERT";
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: return "SELF_SIGNED_CERT_IN_CHAIN";
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: return "UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return "UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        case X509_V_ERR_CERT_CHAIN_TOO_LONG: return "CERT_CHAIN_TOO_LONG";
        case X509_V_ERR_CERT_REVOKED: return "CERT_REVOKED";
        case X509_V_ERR_INVALID_CA: return "INVALID_CA";
        case X509_V_ERR_PATH_LENGTH_EXCEEDED: return "PATH_LENGTH_EXCEEDED";
        case X509_V_ERR_INVALID_PURPOSE: return "INVALID_PURPOSE";
        case X509_V_ERR_CERT_UNTRUSTED: return "CERT_UNTRUSTED";
        case X509_V_ERR_CERT_REJECTED: return "CERT_REJECTED";
        case X509_V_ERR_HOSTNAME_MISMATCH: return "HOSTNAME_MISMATCH";
        default: return nullptr;
    }
}

} // namespace

struct TlsChannel::Impl {
    TlsRole role_ {TlsRole::client};
    SSL_CTX* ctx_ {nullptr};
    SSL* ssl_ {nullptr};
    BIO* rbio_ {nullptr}; // network -> SSL (ciphertext we received)
    BIO* wbio_ {nullptr}; // SSL -> network (ciphertext to send)
    HandshakeState state_ {HandshakeState::not_started};
    IoWant want_ {IoWant::none};
    bool shutdownSent_ {false};
    bool shutdownDone_ {false};
    std::string error_ {};
    std::string errorCode_ {}; // node-style error.code for the last SSL failure
    std::string serverName_ {}; // client SNI / hostname under verification
    std::string alpnWire_ {}; // server: length-prefixed ALPN list for the select cb

    Impl() = default;

    // Server ALPN selection (SSL_CTX_set_alpn_select_cb). Server preference:
    // the first protocol in our list that the client also offered. No match →
    // NOACK (handshake proceeds without ALPN). Mirrors bun's alpn_select_proto_cb
    // (bun-zig-src/src/uws_sys/ssl.zig) which uses server-preference selection.
    static int alpn_select_(SSL*, const unsigned char** out, unsigned char* outlen,
                            const unsigned char* in, unsigned int inlen, void* arg) {
        const std::string& srv {static_cast<Impl*>(arg)->alpnWire_};
        for (std::size_t i {0}; i < srv.size();) {
            const auto sl {static_cast<unsigned>(static_cast<unsigned char>(srv[i]))};
            if (i + 1 + sl > srv.size()) break;
            const auto* sp {reinterpret_cast<const unsigned char*>(srv.data() + i + 1)};
            for (unsigned j {0}; j < inlen;) {
                const unsigned cl {in[j]};
                if (j + 1 + cl > inlen) break;
                if (cl == sl && std::memcmp(sp, in + j + 1, sl) == 0) {
                    *out = sp;
                    *outlen = static_cast<unsigned char>(sl);
                    return SSL_TLSEXT_ERR_OK;
                }
                j += 1 + cl;
            }
            i += 1 + sl;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }

    // Build the length-prefixed ALPN wire form ("\x02h2\x08http/1.1") from names.
    static std::string alpn_wire_(const std::vector<std::string>& protos) {
        std::string wire;
        for (const auto& p : protos) {
            if (p.empty() || p.size() > 255) continue;
            wire.push_back(static_cast<char>(p.size()));
            wire.append(p);
        }
        return wire;
    }

    ~Impl() {
        if (ssl_ != nullptr) {
            ::SSL_free(ssl_); // frees the attached rbio_/wbio_
        }
        if (ctx_ != nullptr) {
            ::SSL_CTX_free(ctx_);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void fail_(std::string_view where) {
        state_ = HandshakeState::failed;
        // Capture the node-style code from the topmost error BEFORE draining
        // (drain empties the queue). Keep the first code seen so an early fatal
        // reason isn't overwritten by a later teardown error.
        if (errorCode_.empty()) {
            if (const unsigned long peeked {::ERR_peek_last_error()}; peeked != 0) {
                errorCode_ = node_error_code(peeked);
            }
        }
        std::string detail {drain_openssl_errors()};
        // node runs the chain verification in OpenSSL but the *hostname* check
        // in JS (tls.checkServerIdentity), so a SAN mismatch surfaces as
        // ERR_TLS_CERT_ALTNAME_INVALID / "Hostname/IP does not match
        // certificate's altnames: ...", not the generic
        // "certificate verify failed". SSL_set1_host() folds that check into
        // the verify result, so translate the code back to node's wording.
        if (ssl_ != nullptr) {
            const long vr {::SSL_get_verify_result(ssl_)};
            if (vr == X509_V_ERR_HOSTNAME_MISMATCH || vr == X509_V_ERR_IP_ADDRESS_MISMATCH) {
                errorCode_ = "ERR_TLS_CERT_ALTNAME_INVALID";
                error_ = "Hostname/IP does not match certificate's altnames: Host: " +
                         serverName_ + ". is not in the cert's altnames";
                return;
            }
            // A CHAIN verification failure is reported by node as the X509 error
            // NAME (the OpenSSL macro minus its X509_V_ERR_ prefix) in
            // `error.code`, with OpenSSL's reason string as the message — NOT as
            // the generic "certificate verify failed" the error queue carries.
            // Blueprint: node src/crypto/crypto_common.cc X509ErrorCode().
            // This changes the error's SHAPE only: the handshake still fails.
            if (const char* verifyCode {x509_error_code(vr)}; verifyCode != nullptr) {
                errorCode_ = verifyCode;
                const char* reason {::X509_verify_cert_error_string(vr)};
                error_ = reason != nullptr ? reason : verifyCode;
                return;
            }
        }
        error_ = std::string {where};
        if (!detail.empty()) {
            error_ += ": ";
            error_ += detail;
        }
    }

    bool setup_(Config config) {
        ensure_library();
        ctx_ = ::SSL_CTX_new(role_ == TlsRole::client ? ::TLS_client_method()
                                                      : ::TLS_server_method());
        if (ctx_ == nullptr) {
            fail_("SSL_CTX_new");
            return false;
        }
        // Protocol version window. Default floor is TLS 1.2 (node's
        // DEFAULT_MIN_VERSION); an explicit config pin overrides either bound.
        // An impossible window (min > max) is not rejected here — SSL_do_handshake
        // surfaces it as SSL_R_NO_SUPPORTED_VERSIONS_ENABLED, which fail_()
        // translates to error.code for node parity.
        ::SSL_CTX_set_min_proto_version(ctx_,
                                        config.minVersion != 0 ? config.minVersion : TLS1_2_VERSION);
        if (config.maxVersion != 0) {
            ::SSL_CTX_set_max_proto_version(ctx_, config.maxVersion);
        }
        // An impossible window (min > max) makes SSL_do_handshake fail. OpenSSL's
        // reason there is "no protocols available"; node ships BoringSSL, whose
        // reason is "no supported versions enabled". Pin the node/BoringSSL code
        // up front so error.code matches node (the failure itself is genuine —
        // the handshake still fails below). fail_() keeps this first code.
        if (config.minVersion != 0 && config.maxVersion != 0
            && config.minVersion > config.maxVersion) {
            errorCode_ = "ERR_SSL_NO_SUPPORTED_VERSIONS_ENABLED";
        }
        ::SSL_CTX_set_verify(ctx_, verify_flags_for(role_, config.verify), nullptr);

        // Trust anchors for chain verification. An explicit PEM bundle in
        // config.ca is loaded verbatim (self-signed test CAs); otherwise, when
        // peers are verified, fall back to the platform default CA store
        // (SSL_CTX_set_default_verify_paths ≈ SSL_CTX_load_verify_locations on
        // OPENSSLDIR / SSL_CERT_FILE / SSL_CERT_DIR — the system bundle), which
        // is what a client needs to trust registry.npmjs.org's chain.
        if (!config.ca.empty()) {
            if (!load_ca_pem_(config.ca)) {
                return false;
            }
        } else if (config.verify != VerifyMode::disabled) {
            if (!configure_default_trust_()) {
                fail_("configure_default_trust: no usable system CA store");
                return false;
            }
        }

        if (!config.certificate.empty() && !use_certificate_(config.certificate)) {
            return false;
        }
        if (!config.key.empty() && !use_private_key_(config.key)) {
            return false;
        }

        // ALPN: the client advertises its list; the server registers a select
        // callback that picks by server preference from the same list.
        if (!config.alpnProtocols.empty()) {
            const std::string wire {alpn_wire_(config.alpnProtocols)};
            if (!wire.empty()) {
                if (role_ == TlsRole::client) {
                    ::SSL_CTX_set_alpn_protos(
                        ctx_, reinterpret_cast<const unsigned char*>(wire.data()),
                        static_cast<unsigned>(wire.size()));
                } else {
                    alpnWire_ = wire;
                    ::SSL_CTX_set_alpn_select_cb(ctx_, &Impl::alpn_select_, this);
                }
            }
        }

        ssl_ = ::SSL_new(ctx_);
        if (ssl_ == nullptr) {
            fail_("SSL_new");
            return false;
        }
        rbio_ = ::BIO_new(::BIO_s_mem());
        wbio_ = ::BIO_new(::BIO_s_mem());
        if (rbio_ == nullptr || wbio_ == nullptr) {
            fail_("BIO_new");
            return false;
        }
        ::SSL_set_bio(ssl_, rbio_, wbio_); // takes ownership of both BIOs

        if (role_ == TlsRole::client) {
            ::SSL_set_connect_state(ssl_);
            serverName_ = config.serverName;
            if (!config.serverName.empty()) {
                // SNI. The cast drops const per the historic macro signature.
                ::SSL_set_tlsext_host_name(ssl_, config.serverName.c_str());
                if (config.verify != VerifyMode::disabled) {
                    ::SSL_set1_host(ssl_, config.serverName.c_str());
                }
            }
        } else {
            ::SSL_set_accept_state(ssl_);
        }
        return true;
    }

    // Discover and load the platform default trust store. OpenSSL's built-in
    // SSL_CTX_set_default_verify_paths() consults the OPENSSLDIR compiled into
    // libcrypto, which for a relocated/prebuilt build points at a path that does
    // not exist on the target host — so the leaf verify fails with "system
    // library: No such file or directory". We therefore honour the standard
    // SSL_CERT_FILE / SSL_CERT_DIR overrides first, then probe the well-known
    // distro CA bundle locations (this is what bun's platform trust discovery
    // resolves to on Linux), and only fall back to the compiled-in default.
    bool configure_default_trust_() {
        if (const char* file {std::getenv("SSL_CERT_FILE")}; file != nullptr && *file != '\0') {
            if (::SSL_CTX_load_verify_locations(ctx_, file, nullptr) == 1) {
                return true;
            }
        }
        if (const char* dir {std::getenv("SSL_CERT_DIR")}; dir != nullptr && *dir != '\0') {
            if (::SSL_CTX_load_verify_locations(ctx_, nullptr, dir) == 1) {
                return true;
            }
        }
        static constexpr std::string_view bundleFiles[] {
            "/etc/ssl/certs/ca-certificates.crt",              // Debian/Ubuntu/Alpine/Gentoo
            "/etc/pki/tls/certs/ca-bundle.crt",                // Fedora/RHEL/CentOS
            "/etc/ssl/ca-bundle.pem",                          // OpenSUSE
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/ssl/cert.pem",                               // macOS/BSD/Alpine
        };
        std::error_code ec {};
        for (std::string_view file : bundleFiles) {
            if (std::filesystem::exists(file, ec)
                && ::SSL_CTX_load_verify_locations(ctx_, std::string {file}.c_str(), nullptr) == 1) {
                return true;
            }
        }
        static constexpr std::string_view certDirs[] {"/etc/ssl/certs", "/etc/pki/tls/certs"};
        for (std::string_view dir : certDirs) {
            if (std::filesystem::is_directory(dir, ec)
                && ::SSL_CTX_load_verify_locations(ctx_, nullptr, std::string {dir}.c_str()) == 1) {
                return true;
            }
        }
        return ::SSL_CTX_set_default_verify_paths(ctx_) == 1;
    }

    // Load one or more concatenated PEM certificates as verification trust
    // anchors into the context's X509_STORE (the caller-supplied CA bundle).
    bool load_ca_pem_(const std::string& pem) {
        BIO* bio {::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
        if (bio == nullptr) {
            fail_("BIO_new_mem_buf(ca)");
            return false;
        }
        X509_STORE* store {::SSL_CTX_get_cert_store(ctx_)};
        int added {0};
        X509* cert {nullptr};
        while ((cert = ::PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) != nullptr) {
            if (::X509_STORE_add_cert(store, cert) == 1) {
                ++added;
            }
            ::X509_free(cert);
        }
        ::BIO_free(bio);
        if (added == 0) {
            fail_("load_ca_pem: no certificates in CA bundle");
            return false;
        }
        return true;
    }

    bool use_certificate_(const std::string& pem) {
        BIO* bio {::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
        if (bio == nullptr) {
            fail_("BIO_new_mem_buf(cert)");
            return false;
        }
        X509* cert {::PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
        bool ok {cert != nullptr && ::SSL_CTX_use_certificate(ctx_, cert) == 1};
        if (cert != nullptr) {
            // Append any remaining certs in the PEM as the chain.
            X509* chain {nullptr};
            while ((chain = ::PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) != nullptr) {
                ::SSL_CTX_add_extra_chain_cert(ctx_, chain);
            }
            ::X509_free(cert);
        }
        ::BIO_free(bio);
        if (!ok) {
            fail_("use_certificate");
        }
        return ok;
    }

    bool use_private_key_(const std::string& pem) {
        BIO* bio {::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
        if (bio == nullptr) {
            fail_("BIO_new_mem_buf(key)");
            return false;
        }
        EVP_PKEY* key {::PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr)};
        bool ok {key != nullptr && ::SSL_CTX_use_PrivateKey(ctx_, key) == 1};
        if (key != nullptr) {
            ::EVP_PKEY_free(key);
        }
        ::BIO_free(bio);
        if (!ok) {
            fail_("use_private_key");
        }
        return ok;
    }

    void classify_(int ret) {
        int err {::SSL_get_error(ssl_, ret)};
        switch (err) {
        case SSL_ERROR_NONE:
            state_ = HandshakeState::complete;
            want_ = IoWant::none;
            break;
        // WANT_READ/WANT_WRITE after an established handshake are ordinary
        // "would block" signals from SSL_read/SSL_write (no more app data /
        // socket full) — they must NOT regress a completed handshake back to
        // in_progress (that clobbers established(); read() drains app data then
        // reports WANT_READ on the empty BIO every time). Only advance want_.
        case SSL_ERROR_WANT_READ:
            if (state_ != HandshakeState::complete) state_ = HandshakeState::in_progress;
            want_ = IoWant::read;
            break;
        case SSL_ERROR_WANT_WRITE:
            if (state_ != HandshakeState::complete) state_ = HandshakeState::in_progress;
            want_ = IoWant::write;
            break;
        case SSL_ERROR_ZERO_RETURN:
            want_ = IoWant::none;
            break;
        default:
            fail_("ssl");
            break;
        }
    }
};

TlsChannel::TlsChannel(TlsRole role, Config config) : impl_ {std::make_unique<Impl>()} {
    impl_->role_ = role;
    impl_->setup_(std::move(config));
}

TlsChannel::~TlsChannel() = default;
TlsChannel::TlsChannel(TlsChannel&&) noexcept = default;
TlsChannel& TlsChannel::operator=(TlsChannel&&) noexcept = default;

bool TlsChannel::valid() const noexcept {
    return impl_->ssl_ != nullptr && impl_->state_ != HandshakeState::failed;
}

HandshakeState TlsChannel::handshake() noexcept {
    if (impl_->ssl_ == nullptr || impl_->state_ == HandshakeState::failed
        || impl_->state_ == HandshakeState::complete) {
        return impl_->state_;
    }
    impl_->state_ = HandshakeState::in_progress;
    int ret {::SSL_do_handshake(impl_->ssl_)};
    if (ret == 1) {
        impl_->state_ = HandshakeState::complete;
        impl_->want_ = IoWant::none;
    } else {
        impl_->classify_(ret);
    }
    return impl_->state_;
}

HandshakeState TlsChannel::handshake_state() const noexcept { return impl_->state_; }
bool TlsChannel::established() const noexcept {
    return impl_->state_ == HandshakeState::complete;
}
IoWant TlsChannel::want() const noexcept { return impl_->want_; }
std::string_view TlsChannel::last_error() const noexcept { return impl_->error_; }
std::string_view TlsChannel::error_code() const noexcept { return impl_->errorCode_; }

int tls_version_from_name(std::string_view name) {
    if (name == "TLSv1.3") { return TLS1_3_VERSION; }
    if (name == "TLSv1.2") { return TLS1_2_VERSION; }
    if (name == "TLSv1.1") { return TLS1_1_VERSION; }
    if (name == "TLSv1" || name == "TLSv1.0") { return TLS1_VERSION; }
    return 0;
}

void TlsChannel::feed_encrypted(std::span<const std::uint8_t> data) {
    if (impl_->rbio_ == nullptr || data.empty()) {
        return;
    }
    ::BIO_write(impl_->rbio_, data.data(), static_cast<int>(data.size()));
}

bool TlsChannel::has_encrypted() const noexcept {
    return impl_->wbio_ != nullptr && ::BIO_ctrl_pending(impl_->wbio_) > 0;
}

std::vector<std::uint8_t> TlsChannel::take_encrypted() {
    std::vector<std::uint8_t> out {};
    if (impl_->wbio_ == nullptr) {
        return out;
    }
    std::uint8_t chunk[4096];
    int n {};
    while ((n = ::BIO_read(impl_->wbio_, chunk, sizeof(chunk))) > 0) {
        out.insert(out.end(), chunk, chunk + n);
    }
    return out;
}

std::size_t TlsChannel::write(std::span<const std::uint8_t> plain) noexcept {
    if (impl_->ssl_ == nullptr || plain.empty()) {
        return 0;
    }
    int n {::SSL_write(impl_->ssl_, plain.data(), static_cast<int>(plain.size()))};
    if (n <= 0) {
        impl_->classify_(n);
        return 0;
    }
    return static_cast<std::size_t>(n);
}

std::vector<std::uint8_t> TlsChannel::read() {
    std::vector<std::uint8_t> out {};
    if (impl_->ssl_ == nullptr) {
        return out;
    }
    std::uint8_t chunk[4096];
    for (;;) {
        int n {::SSL_read(impl_->ssl_, chunk, sizeof(chunk))};
        if (n > 0) {
            out.insert(out.end(), chunk, chunk + n);
            continue;
        }
        impl_->classify_(n);
        break;
    }
    return out;
}

bool TlsChannel::shutdown() noexcept {
    if (impl_->ssl_ == nullptr) {
        return true;
    }
    int ret {::SSL_shutdown(impl_->ssl_)};
    if (ret == 1) {
        impl_->shutdownSent_ = true;
        impl_->shutdownDone_ = true;
    } else if (ret == 0) {
        impl_->shutdownSent_ = true; // our close_notify is out; peer's not seen yet
    } else {
        impl_->classify_(ret);
    }
    return impl_->shutdownDone_;
}

bool TlsChannel::shutdown_complete() const noexcept { return impl_->shutdownDone_; }

std::string TlsChannel::tls_version() const {
    if (impl_->ssl_ == nullptr) {
        return {};
    }
    const char* v {::SSL_get_version(impl_->ssl_)};
    return v != nullptr ? std::string {v} : std::string {};
}

std::string TlsChannel::cipher() const {
    if (impl_->ssl_ == nullptr) {
        return {};
    }
    const SSL_CIPHER* c {::SSL_get_current_cipher(impl_->ssl_)};
    const char* name {c != nullptr ? ::SSL_CIPHER_get_name(c) : nullptr};
    return name != nullptr ? std::string {name} : std::string {};
}

std::string TlsChannel::peer_server_name() const {
    if (impl_->ssl_ == nullptr) {
        return {};
    }
    const char* name {::SSL_get_servername(impl_->ssl_, TLSEXT_NAMETYPE_host_name)};
    return name != nullptr ? std::string {name} : std::string {};
}

std::string TlsChannel::peer_certificate_pem() const {
    if (impl_->ssl_ == nullptr) {
        return {};
    }
    // SSL_get_peer_certificate bumps the refcount — free the returned handle.
    X509* cert {::SSL_get_peer_certificate(impl_->ssl_)};
    if (cert == nullptr) {
        return {};
    }
    std::string out;
    if (BIO* bio {::BIO_new(::BIO_s_mem())}) {
        if (::PEM_write_bio_X509(bio, cert) == 1) {
            char* data {nullptr};
            const long n {::BIO_get_mem_data(bio, &data)};
            if (data != nullptr && n > 0) {
                out.assign(data, static_cast<std::size_t>(n));
            }
        }
        ::BIO_free(bio);
    }
    ::X509_free(cert);
    return out;
}

bool TlsChannel::verify_ok() const noexcept {
    if (impl_->ssl_ == nullptr) {
        return false;
    }
    return ::SSL_get_verify_result(impl_->ssl_) == X509_V_OK;
}

std::string TlsChannel::alpn_protocol() const {
    if (impl_->ssl_ == nullptr) {
        return {};
    }
    const unsigned char* data {nullptr};
    unsigned len {0};
    ::SSL_get0_alpn_selected(impl_->ssl_, &data, &len);
    return (data != nullptr && len > 0)
               ? std::string {reinterpret_cast<const char*>(data), len}
               : std::string {};
}

std::optional<CertKeyPem> make_self_signed(std::string_view commonName) {
    ensure_library();

    EVP_PKEY* pkey {::EVP_RSA_gen(2048)};
    if (pkey == nullptr) {
        return std::nullopt;
    }
    X509* x509 {::X509_new()};
    if (x509 == nullptr) {
        ::EVP_PKEY_free(pkey);
        return std::nullopt;
    }

    ::X509_set_version(x509, 2); // X509 v3
    ::ASN1_INTEGER_set(::X509_get_serialNumber(x509), 1);
    ::X509_gmtime_adj(::X509_getm_notBefore(x509), 0);
    ::X509_gmtime_adj(::X509_getm_notAfter(x509), 60L * 60L * 24L * 365L);
    ::X509_set_pubkey(x509, pkey);

    X509_NAME* name {::X509_get_subject_name(x509)};
    std::string cn {commonName};
    ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(cn.c_str()),
                                 -1, -1, 0);
    ::X509_set_issuer_name(x509, name); // self-signed: issuer == subject

    std::optional<CertKeyPem> result {};
    if (::X509_sign(x509, pkey, ::EVP_sha256()) != 0) {
        CertKeyPem pem {};

        BIO* certBio {::BIO_new(::BIO_s_mem())};
        if (certBio != nullptr && ::PEM_write_bio_X509(certBio, x509) == 1) {
            char* data {nullptr};
            long len {::BIO_get_mem_data(certBio, &data)};
            pem.cert.assign(data, static_cast<std::size_t>(len));
        }
        ::BIO_free(certBio);

        BIO* keyBio {::BIO_new(::BIO_s_mem())};
        if (keyBio != nullptr
            && ::PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr)
                   == 1) {
            char* data {nullptr};
            long len {::BIO_get_mem_data(keyBio, &data)};
            pem.key.assign(data, static_cast<std::size_t>(len));
        }
        ::BIO_free(keyBio);

        if (!pem.cert.empty() && !pem.key.empty()) {
            result = std::move(pem);
        }
    }

    ::X509_free(x509);
    ::EVP_PKEY_free(pkey);
    return result;
}

} // namespace mbun::tls
