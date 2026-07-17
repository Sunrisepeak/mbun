// Backend seam modeled after Bun's EVP/HMAC/PBKDF2/password runtime crypto
// files. Native BoringSSL/JSC wiring is intentionally injectable and deferred.
export module mbun.runtime_crypto.backend;

import std;

export namespace mbun::runtime_crypto {

enum class BackendOperation : std::uint8_t {
    digest,
    hmac,
    pbkdf2,
    password,
};

enum class BackendErrorKind : std::uint8_t {
    unavailable,
    invalid_request,
};

struct BackendError {
    BackendErrorKind kind { BackendErrorKind::unavailable };
    std::string operation {};
    std::string detail {};
};

struct BackendCapabilities {
    bool digest { false };
    bool hmac { false };
    bool pbkdf2 { false };
    bool password { false };

    bool supports(BackendOperation operation) const {
        switch (operation) {
        case BackendOperation::digest: return digest;
        case BackendOperation::hmac: return hmac;
        case BackendOperation::pbkdf2: return pbkdf2;
        case BackendOperation::password: return password;
        }
        return false;
    }
};

class CryptoBackend {
public:
    virtual ~CryptoBackend() = default;
    virtual BackendCapabilities capabilities() const = 0;
};

class DeferredBackend final : public CryptoBackend {
public:
    BackendCapabilities capabilities() const override { return {}; }

    static BackendError unavailable(BackendOperation operation) {
        auto name { operation_name_(operation) };
        return {BackendErrorKind::unavailable, std::string { name },
                "native BoringSSL/JSC backend is deferred"};
    }

private:
    static std::string_view operation_name_(BackendOperation operation) {
        switch (operation) {
        case BackendOperation::digest: return "digest";
        case BackendOperation::hmac: return "hmac";
        case BackendOperation::pbkdf2: return "pbkdf2";
        case BackendOperation::password: return "password";
        }
        return "unknown";
    }
};

class PureDigestBackend final : public CryptoBackend {
public:
    BackendCapabilities capabilities() const override { return {.digest = true}; }
};

}  // namespace mbun::runtime_crypto
