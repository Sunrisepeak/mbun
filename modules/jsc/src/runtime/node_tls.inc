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

// Install globalThis.__mbunNodeTlsNative. Called once from engine init.
inline void install_node_tls_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef tn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, tn, "getCiphers", node_tls_get_ciphers_cb);
    set_fn(ctx, tn, "canonicalizeIP", node_tls_canonicalize_ip_cb);
    set_prop(ctx, global, "__mbunNodeTlsNative", tn);
}
