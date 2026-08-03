// WebCrypto HMAC private-key bridge.
// ref: bun src/jsc/bindings/webcrypto/{CryptoKey.cpp,CryptoKeyHMAC.cpp,
//      CryptoAlgorithmHMAC.cpp,JSSubtleCrypto.cpp}

enum class WebCryptoKeyUsage : std::uint8_t {
    Sign = 1U << 0U,
    Verify = 1U << 1U,
};

constexpr std::uint8_t usage_bit(WebCryptoKeyUsage usage) {
    return std::to_underlying(usage);
}

struct WebCryptoKeyState {
    mbun::crypto::DigestAlgo hash;
    std::vector<std::uint8_t> bytes;
    std::uint8_t usages{};
    bool extractable{};
};

JSClassRef gCryptoKeyClass{nullptr};

void webcrypto_key_finalize(JSObjectRef object) {
    auto* state{static_cast<WebCryptoKeyState*>(JSObjectGetPrivate(object))};
    if (state == nullptr) return;
    // Keep the wipe observable to the compiler before releasing key storage.
    volatile std::uint8_t* bytes{state->bytes.data()};
    for (std::size_t i{}; i < state->bytes.size(); ++i) bytes[i] = 0;
    delete state;
}

JSObjectRef webcrypto_key_construct_cb(JSContextRef ctx, JSObjectRef, std::size_t,
                                       const JSValueRef[], JSValueRef* exception) {
    // Ctor cbs run under DropAllLocks; keep the locked-callback invariant
    // uniform across constructors (see set_fn in common.inc).
    JSC::JSLockHolder locker{toJS(ctx)};
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSObjectRef typeError{
        JSValueToObject(ctx, get_prop(ctx, global, "TypeError"), nullptr)};
    JSValueRef message{make_string(ctx, "Illegal constructor")};
    JSValueRef constructionException{nullptr};
    JSObjectRef error{
        JSObjectCallAsConstructor(ctx, typeError, 1, &message, &constructionException)};
    if (exception != nullptr) {
        *exception = constructionException != nullptr ? constructionException
                                                      : static_cast<JSValueRef>(error);
    }
    return nullptr;
}

WebCryptoKeyState* webcrypto_key_state(JSC::JSGlobalObject* global, JSC::JSValue value) {
    if (!value.isObject()) return nullptr;
    JSObjectRef object{toRef(JSC::asObject(value))};
    if (!JSValueIsObjectOfClass(toRef(global), object, gCryptoKeyClass)) return nullptr;
    return static_cast<WebCryptoKeyState*>(JSObjectGetPrivate(object));
}

bool webcrypto_key_allows(const WebCryptoKeyState& state, WebCryptoKeyUsage usage) {
    return (state.usages & usage_bit(usage)) != 0U;
}

JSC_DEFINE_HOST_FUNCTION(webcrypto_import_hmac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JsStrArg algorithmName{global, native_arg(frame, 0)};
    const auto algorithm{mbun::crypto::digest_algo_from(algorithmName.view())};
    if (!algorithm) {
        return hasher_throw(global, "WebCrypto HMAC: unsupported hash algorithm");
    }

    std::vector<std::uint8_t> keyBytes;
    const bool keyOk{with_hasher_bytes(
        global, native_arg(frame, 1),
        [&](std::span<const std::uint8_t> bytes) { keyBytes.assign(bytes.begin(), bytes.end()); })};
    const JSC::JSValue usagesValue{native_arg(frame, 3)};
    if (!keyOk || keyBytes.empty() || !usagesValue.isNumber()) {
        return hasher_throw(global, "WebCrypto HMAC: invalid key material");
    }

    const auto usages{static_cast<std::uint8_t>(usagesValue.asNumber())};
    constexpr std::uint8_t ALLOWED_USAGES{
        usage_bit(WebCryptoKeyUsage::Sign) | usage_bit(WebCryptoKeyUsage::Verify)};
    if (usages == 0U || (usages & ~ALLOWED_USAGES) != 0U) {
        return hasher_throw(global, "WebCrypto HMAC: invalid key usages");
    }

    auto* state{new WebCryptoKeyState{
        *algorithm, std::move(keyBytes), usages, native_arg(frame, 2).toBoolean(global)}};
    JSObjectRef key{JSObjectMake(toRef(global), gCryptoKeyClass, state)};
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(key)));
}

JSC_DEFINE_HOST_FUNCTION(webcrypto_hmac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    WebCryptoKeyState* key{webcrypto_key_state(global, native_arg(frame, 0))};
    if (key == nullptr || !webcrypto_key_allows(*key, WebCryptoKeyUsage::Sign)) {
        return hasher_throw(global, "WebCrypto HMAC: key does not allow sign");
    }

    std::optional<std::vector<std::uint8_t>> mac;
    const bool dataOk{with_hasher_bytes(
        global, native_arg(frame, 1), [&](std::span<const std::uint8_t> data) {
            mac = mbun::crypto::hmac(key->hash, key->bytes, data);
        })};
    if (!dataOk || !mac) {
        return hasher_throw(global, "WebCrypto HMAC: data must be a BufferSource value");
    }
    return JSC::JSValue::encode(make_u8array(global, *mac));
}

JSC_DEFINE_HOST_FUNCTION(webcrypto_verify_hmac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    WebCryptoKeyState* key{webcrypto_key_state(global, native_arg(frame, 0))};
    if (key == nullptr || !webcrypto_key_allows(*key, WebCryptoKeyUsage::Verify)) {
        return hasher_throw(global, "WebCrypto HMAC: key does not allow verify");
    }

    bool matches{false};
    bool complete{false};
    const bool signatureOk{with_hasher_bytes(
        global, native_arg(frame, 1), [&](std::span<const std::uint8_t> signature) {
            const bool dataOk{with_hasher_bytes(
                global, native_arg(frame, 2), [&](std::span<const std::uint8_t> data) {
                    const auto expected{mbun::crypto::hmac(key->hash, key->bytes, data)};
                    complete = expected.has_value();
                    matches = expected && mbun::crypto::constant_time_equal(*expected, signature);
                })};
            if (!dataOk) complete = false;
        })};
    if (!signatureOk || !complete) {
        return hasher_throw(
            global, "WebCrypto HMAC: signature and data must be BufferSource values");
    }
    return JSC::JSValue::encode(JSC::jsBoolean(matches));
}
