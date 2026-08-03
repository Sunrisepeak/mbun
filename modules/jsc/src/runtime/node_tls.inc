// runtime/node_tls.inc — node:tls native backend over the vendored OpenSSL
// 3.1.5 static lib (mbun.openssl local-index package; libssl.a + libcrypto.a,
// NOT host /usr). Exposes globalThis.__mbunNodeTlsNative for the JS layer
// (builtins/node_tls.cppm) to build tls.getCiphers() on top of a real
// SSL_CTX/SSL default cipher list.
//
// Certificate parsing for the JS layer (createSecureContext / X509 fields /
// checkServerIdentity input) reuses the already-installed
// globalThis.__mbunCryptoAsymNative.x509parse (runtime/crypto_asym.inc, same
// mbun.openssl X509 API) — no duplicate parser here.
//
// ref: bun-ref src/js/node/tls.ts getCiphers()/getDefaultCiphers().split(":").
// DEFERRED: the SSL_CTX-backed SecureContext handle + live handshake surface
// (tls.connect / tls.Server) — those need the socket event loop.

// __mbunNodeTlsNative.getCiphers() → string[] of the default enabled TLS cipher
// suites (lowercased, matching node's tls.getCiphers()). Read straight off a
// throwaway default SSL object so it reflects the linked OpenSSL build.
inline JSValueRef node_tls_get_ciphers_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                          std::size_t, const JSValueRef[], JSValueRef*) {
    std::vector<JSValueRef> out;
    if (const SSL_METHOD* method{TLS_method()}) {
        if (SSL_CTX* c{SSL_CTX_new(method)}) {
            if (SSL* s{SSL_new(c)}) {
                for (int i{0};; ++i) {
                    const char* name{SSL_get_cipher_list(s, i)};
                    if (name == nullptr) { break; }
                    std::string lower{name};
                    for (char& ch : lower) {
                        if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); }
                    }
                    out.push_back(make_string(ctx, lower));
                }
                SSL_free(s);
            }
            SSL_CTX_free(c);
        }
    }
    return JSObjectMakeArray(ctx, out.size(), out.empty() ? nullptr : out.data(), nullptr);
}

// __mbunNodeTlsNative.canonicalizeIP(text) → the address's canonical text form,
// or undefined when it is not an IP literal. Backs node:tls checkServerIdentity's
// IP-SAN comparison (an `IP Address:` SAN and the hostname must be compared in
// canonical form, else "fe80:0:0:0:0:0:0:1" and "fe80::1" read as different
// hosts) and bun:internal-for-testing's canonicalizeIP.
//
// Blueprint: bun runtime/dns_jsc/cares_jsc.rs:822-853 Bun__canonicalizeIP →
// boringssl/lib.rs:242-271 canonicalize_ip — reject anything at/over
// INET6_ADDRSTRLEN, try inet_pton(AF_INET) first and fall back to AF_INET6, then
// render with inet_ntop. A '/' (CIDR) is rejected up front (cares_jsc.rs:840).
// The pton/ntop round trip — NOT the RFC 5952 formatter in mbun.dns.address — is
// what produces the IPv4-mapped form "::ffff:192.168.1.1" for
// "0:0:0:0:0:ffff:c0a8:101", so this deliberately uses the libc pair.
inline JSValueRef node_tls_canonicalize_ip_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                              std::size_t argc, const JSValueRef args[],
                                              JSValueRef* exc) {
    if (argc < 1) {
        if (exc != nullptr) {
            *exc = make_named_error(
                ctx, "TypeError", "canonicalizeIP() expects a string but received no arguments.");
        }
        return JSValueMakeUndefined(ctx);
    }
    const std::string addr{arg_string(ctx, argc, args, 0)};
    if (addr.empty() || addr.size() >= INET6_ADDRSTRLEN || addr.find('/') != std::string::npos) {
        return JSValueMakeUndefined(ctx);
    }
    std::array<unsigned char, sizeof(in6_addr)> raw{};
    int af{AF_INET};
    if (::inet_pton(AF_INET, addr.c_str(), raw.data()) != 1) {
        af = AF_INET6;
        if (::inet_pton(AF_INET6, addr.c_str(), raw.data()) != 1) {
            return JSValueMakeUndefined(ctx);
        }
    }
    std::array<char, INET6_ADDRSTRLEN + 1> out{};
    if (::inet_ntop(af, raw.data(), out.data(), out.size() - 1) == nullptr) {
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, out.data());
}

// __mbunNodeTlsNative.checkCipherList(str) → does OpenSSL accept this cipher
// list? node's SecureContext::SetCiphers throws ERR_SSL_NO_CIPHER_MATCH when
// SSL_CTX_set_cipher_list() fails, which is the only way to learn that a list
// like "aes256-sha" or "FOOBARBAZ" selects nothing. Missing/empty argument
// answers true so only an explicit list is judged.
inline JSValueRef node_tls_check_cipher_list_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                                std::size_t argc, const JSValueRef argv[],
                                                JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, true);
    const std::string list{val_to_string(ctx, argv[0])};
    bool ok{true};
    if (const SSL_METHOD* method{TLS_method()}) {
        if (SSL_CTX* c{SSL_CTX_new(method)}) {
            ok = SSL_CTX_set_cipher_list(c, list.c_str()) == 1;
            SSL_CTX_free(c);
        }
    }
    // OpenSSL leaves the failure on the thread error queue; clear it so a later
    // unrelated call does not report it.
    if (!ok) ERR_clear_error();
    return JSValueMakeBoolean(ctx, ok);
}

// __mbunNodeTlsNative.checkCipherSuites(str) → does OpenSSL accept this TLS 1.3
// suite list? The 1.3 suites sit in a separate slot from the cipher list
// (SSL_CTX_set_ciphersuites, not SSL_CTX_set_cipher_list), so validating them
// with the cipher-list setter rejects every legitimate TLS_* name. node's
// SecureContext::SetCipherSuites raises the same ERR_SSL_NO_CIPHER_MATCH shape
// when the setter fails.
inline JSValueRef node_tls_check_cipher_suites_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                                  std::size_t argc, const JSValueRef argv[],
                                                  JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, true);
    const std::string list{val_to_string(ctx, argv[0])};
    bool ok{true};
    if (const SSL_METHOD* method{TLS_method()}) {
        if (SSL_CTX* c{SSL_CTX_new(method)}) {
            ok = SSL_CTX_set_ciphersuites(c, list.c_str()) == 1;
            SSL_CTX_free(c);
        }
    }
    if (!ok) ERR_clear_error();
    return JSValueMakeBoolean(ctx, ok);
}

// __mbunNodeTlsNative.dhParamBits(pem) → the prime size in bits of a PEM DH
// parameter block, or 0 when the argument is not one. PORT-SOURCE:
// compat/node/src/crypto/crypto_context.cc SecureContext::SetDHParam, which
// reads BN_num_bits(p) and REFUSES anything under 1024 bits at
// createSecureContext() time ("DH parameter is less than 1024 bits") while only
// warning between 1024 and 2048. That refusal is a security floor, not a
// formatting check: a 512-bit group is breakable, and accepting it silently
// would have every DHE handshake on that server negotiate it. Reporting the
// size here lets the JS layer raise node's error at node's moment — before a
// connection exists — instead of discovering it as a handshake failure.
inline JSValueRef node_tls_dh_param_bits_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                            std::size_t argc, const JSValueRef argv[],
                                            JSValueRef*) {
    if (argc < 1) return JSValueMakeNumber(ctx, 0);
    const std::string pem{val_to_string(ctx, argv[0])};
    int bits{0};
    if (BIO* bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))}) {
        if (EVP_PKEY* params{PEM_read_bio_Parameters(bio, nullptr)}) {
            BIGNUM* p{nullptr};
            if (EVP_PKEY_get_bn_param(params, OSSL_PKEY_PARAM_FFC_P, &p) == 1 && p != nullptr) {
                bits = BN_num_bits(p);
                BN_free(p);
            }
            EVP_PKEY_free(params);
        }
        BIO_free(bio);
    }
    // A non-DH argument leaves a parse reason queued; it is not an error here
    // (0 says "not DH parameters"), so it must not surface on a later call.
    ERR_clear_error();
    return JSValueMakeNumber(ctx, bits);
}

// __mbunNodeTlsNative.rootCertificates() → string[] of PEM certificates from the
// platform trust store — the store mbun actually verifies chains against. node
// answers tls.rootCertificates from its vendored Mozilla NSS bundle
// (src/node_root_certs.h); mbun has no vendored bundle, so reporting the live
// store is the only honest answer. This READS the store, it never adds to it, so
// no chain becomes trusted that was not already.
inline JSValueRef node_tls_root_certificates_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                                std::size_t, const JSValueRef[], JSValueRef*) {
    const std::vector<std::string> roots{mbun::tls::platform_root_certificates()};
    std::vector<JSValueRef> out;
    out.reserve(roots.size());
    for (const std::string& pem : roots) out.push_back(make_string(ctx, pem));
    return JSObjectMakeArray(ctx, out.size(), out.empty() ? nullptr : out.data(), nullptr);
}

// __mbunNodeTlsNative.checkKeyCert(certPem, keyPem, passphrase, ciphers) → ""
// when the pair loads, else the OpenSSL reason string. node's
// SecureContext::SetKey / SetCert fail at createSecureContext() time on a
// mismatched pair or a wrong passphrase (test-tls-key-mismatch,
// test-tls-passphrase); without this the mismatch only surfaced as a handshake
// failure much later. `ciphers` is applied to the probe context BEFORE the cert,
// mirroring node's Init-then-SetCert order, so an `@SECLEVEL=n` in the caller's
// cipher list governs whether the key is accepted
// (test-tls-reduced-SECLEVEL-in-cipher).
inline JSValueRef node_tls_check_key_cert_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                             std::size_t argc, const JSValueRef argv[],
                                             JSValueRef*) {
    const std::string cert{argc > 0 ? val_to_string(ctx, argv[0]) : std::string{}};
    const std::string key{argc > 1 ? val_to_string(ctx, argv[1]) : std::string{}};
    const std::string pass{argc > 2 && JSValueIsString(ctx, argv[2])
                               ? val_to_string(ctx, argv[2])
                               : std::string{}};
    const std::string ciphers{argc > 3 && JSValueIsString(ctx, argv[3])
                                  ? val_to_string(ctx, argv[3])
                                  : std::string{}};
    return make_string(ctx, mbun::tls::check_key_cert_pair(cert, key, pass, ciphers));
}

// __mbunNodeTlsNative.pfxParse(pfx, passphrase) → { cert, key, ca: string[] },
// the PEM credentials inside a PKCS#12 archive (node's `pfx` option). Throws
// node's own error for an archive that cannot be opened — see the taxonomy on
// mbun::tls::parse_pfx (modules/tls/src/openssl.cppm), which this only shuttles.
//
// The JS layer cannot do this: PKCS#12 is a DER/ASN.1 container whose contents
// are password-encrypted, and mbun already links the OpenSSL that owns
// PKCS12_parse. Reimplementing an ASN.1 + PBES parser in JS to feed a C++ TLS
// stack would be the wrong side of the interface line.
//
// `pfx` arrives as a Buffer/Uint8Array (node's option is a BufferSource); a
// string argument is rejected rather than guessed at, because a PFX is binary
// and any text encoding of it would silently corrupt the DER.
inline JSValueRef node_tls_pfx_parse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                        std::size_t argc, const JSValueRef argv[],
                                        JSValueRef* exc) {
    const std::uint8_t* bytes{nullptr};
    std::size_t length{0};
    if (argc > 0) {
        const JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, argv[0], nullptr)};
        if (tt != kJSTypedArrayTypeNone) {
            if (JSObjectRef obj{JSValueToObject(ctx, argv[0], nullptr)}) {
                if (tt == kJSTypedArrayTypeArrayBuffer) {
                    bytes = static_cast<const std::uint8_t*>(
                        JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
                    length = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
                } else {
                    // JSObjectGetTypedArrayBytesPtr answers the BUFFER base, not
                    // the view's first byte: a Buffer that is a slice of a pooled
                    // allocation (which every small Buffer is) would otherwise be
                    // read from the wrong offset.
                    bytes = static_cast<const std::uint8_t*>(
                        JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
                    length = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
                    if (bytes != nullptr) {
                        bytes += JSObjectGetTypedArrayByteOffset(ctx, obj, nullptr);
                    }
                }
            }
        }
    }
    if (bytes == nullptr || length == 0) {
        if (exc != nullptr) {
            JSValueRef m{make_string(ctx, "Unable to load PFX certificate")};
            JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
            set_prop(ctx, err, "code", make_string(ctx, "ERR_CRYPTO_OPERATION_FAILED"));
            *exc = err;
        }
        return JSValueMakeUndefined(ctx);
    }
    const std::string pass{argc > 1 && JSValueIsString(ctx, argv[1])
                               ? val_to_string(ctx, argv[1])
                               : std::string{}};
    const mbun::tls::PfxCredentials creds{
        mbun::tls::parse_pfx(std::span<const std::uint8_t>{bytes, length}, pass)};
    if (!creds.errorMessage.empty()) {
        if (exc != nullptr) {
            JSValueRef m{make_string(ctx, creds.errorMessage)};
            JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
            if (!creds.errorCode.empty()) {
                set_prop(ctx, err, "code", make_string(ctx, creds.errorCode));
            }
            *exc = err;
        }
        return JSValueMakeUndefined(ctx);
    }
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "cert", make_string(ctx, creds.certPem));
    set_prop(ctx, out, "key", make_string(ctx, creds.keyPem));
    std::vector<JSValueRef> cas;
    cas.reserve(creds.caPems.size());
    for (const std::string& pem : creds.caPems) cas.push_back(make_string(ctx, pem));
    set_prop(ctx, out, "ca",
             JSObjectMakeArray(ctx, cas.size(), cas.empty() ? nullptr : cas.data(), nullptr));
    return out;
}

// Install globalThis.__mbunNodeTlsNative. Called once from engine init.
inline void install_node_tls_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef tn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, tn, "getCiphers", node_tls_get_ciphers_cb);
    set_fn(ctx, tn, "checkCipherList", node_tls_check_cipher_list_cb);
    set_fn(ctx, tn, "checkCipherSuites", node_tls_check_cipher_suites_cb);
    set_fn(ctx, tn, "canonicalizeIP", node_tls_canonicalize_ip_cb);
    set_fn(ctx, tn, "rootCertificates", node_tls_root_certificates_cb);
    set_fn(ctx, tn, "checkKeyCert", node_tls_check_key_cert_cb);
    set_fn(ctx, tn, "pfxParse", node_tls_pfx_parse_cb);
    set_fn(ctx, tn, "dhParamBits", node_tls_dh_param_bits_cb);
    set_prop(ctx, global, "__mbunNodeTlsNative", tn);
}
