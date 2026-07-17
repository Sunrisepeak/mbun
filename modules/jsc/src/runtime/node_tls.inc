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

// Install globalThis.__mbunNodeTlsNative. Called once from engine init.
inline void install_node_tls_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef tn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, tn, "getCiphers", node_tls_get_ciphers_cb);
    set_prop(ctx, global, "__mbunNodeTlsNative", tn);
}
