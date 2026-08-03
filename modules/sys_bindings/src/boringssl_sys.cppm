export module mbun.sys_bindings.boringssl_sys;

import std;

namespace mbun::boringssl_sys {

// Ref: bun-ref/src/boringssl_sys/boringssl.rs and the generated Zig C API.
// Opaque types preserve C ABI ownership without copying generated bindgen output.
export struct SSL_CTX;
export struct SSL;
export struct X509;
export struct ASN1_OCTET_STRING;

export constexpr int SSL_VERIFY_NONE = 0;
export constexpr int SSL_VERIFY_PEER = 1;
export constexpr int NID_subject_alt_name = 85;

export struct Api {
    virtual ~Api() = default;
    virtual int crypto_library_init() = 0;
    virtual SSL_CTX* new_client_context() = 0;
    virtual SSL* new_ssl(SSL_CTX*) = 0;
    virtual void free_ssl(SSL*) = 0;
};

export constexpr bool constant_time_equal(std::span<const std::byte> left,
                                          std::span<const std::byte> right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference {0};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

} // namespace mbun::boringssl_sys
