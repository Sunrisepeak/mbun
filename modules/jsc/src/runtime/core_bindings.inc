// ── Bun.semver ──────────────────────────────────────────────────────────────

// ref: bun src/semver_jsc/SemverObject.rs. Both host functions front-load the
// same three guards before touching the grammar, and mbun.semver owns the
// grammar itself: arity (< 2 args throws), a non-ASCII short-circuit (bun's
// parser is byte-oriented, so it declines rather than transcodes), then
// per-function validity. JsStrArg has already coerced each argument to a UTF-8
// string, matching bun's `to_js_string` + `to_slice`.
bool semver_is_ascii(std::string_view s) {
    return std::ranges::none_of(
        s, [](char c) { return static_cast<unsigned char>(c) & 0x80U; });
}

JSC_DEFINE_HOST_FUNCTION(semver_order_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    if (frame->argumentCount() < 2) {
        return throw_plain_error(global, "Expected two arguments");
    }
    JsStrArg a{global, native_arg(frame, 0)};
    JsStrArg b{global, native_arg(frame, 1)};
    if (!semver_is_ascii(a.view()) || !semver_is_ascii(b.view())) {
        return JSC::JSValue::encode(JSC::jsNumber(0));
    }
    // Unlike satisfies, order REJECTS unorderable input instead of coercing it.
    // The trailing newline is bun's (`format!("Invalid SemVer: {}\n", ..)`).
    for (const std::string_view operand : {a.view(), b.view()}) {
        if (!mbun::semver::is_valid(operand)) {
            return throw_plain_error(global, std::format("Invalid SemVer: {}\n", operand));
        }
    }
    return JSC::JSValue::encode(JSC::jsNumber(mbun::semver::order(a.view(), b.view())));
}
JSC_DEFINE_HOST_FUNCTION(semver_satisfies_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    if (frame->argumentCount() < 2) {
        return throw_plain_error(global, "Expected two arguments");
    }
    JsStrArg v{global, native_arg(frame, 0)};
    JsStrArg r{global, native_arg(frame, 1)};
    // satisfies never throws on unparsable input — it just does not match.
    if (!semver_is_ascii(v.view()) || !semver_is_ascii(r.view())) {
        return JSC::JSValue::encode(JSC::jsBoolean(false));
    }
    return JSC::JSValue::encode(JSC::jsBoolean(mbun::semver::satisfies(v.view(), r.view())));
}

// ── Bun.hash (mbun.crypto non-crypto hash family) ───────────────────────────
//
// Bun.hash(input, seed?) is wyhash and returns a BigInt; the sub-functions
// (Bun.hash.wyhash / crc32 / cityHash32/64 / adler32 / xxHash32/64/3 /
// murmur32v2/v3 / murmur64v2 / rapidhash) mirror HashObject.rs: each absorbs its
// own seed-width/order quirk and returns either a u32 JS number or a u64 BigInt.
// xxHash3 is the scalar XXH3_64bits reference (mbun.crypto); the u32-truncated
// seed matches HashObject's @truncate.
//
// Input decode = bun's StringOrBuffer: a JS string is hashed as its UTF-8 bytes
// (zero-copy view via JsStrArg); a TypedArray/Buffer is hashed over its backing
// store. The byte span is valid only for the callback's duration.
template <class F>
JSC::EncodedJSValue with_hash_bytes(JSC::JSGlobalObject* global, JSC::JSValue v, F&& fn) {
    if (v && v.isCell()) {
        if (auto* view{WTF::dynamicDowncast<JSC::JSArrayBufferView>(v.asCell())}) {
            if (view->isDetached() || view->vector() == nullptr) {
                return fn(std::span<const std::uint8_t>{});
            }
            return fn(std::span<const std::uint8_t>{
                static_cast<const std::uint8_t*>(view->vector()), view->byteLength()});
        }
    }
    JsStrArg s{global, v};
    std::string_view sv{s.view()};
    return fn(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()});
}

// Seed decode: a BigInt seed keeps its full 64 bits (heap bigint, since the
// prebuilt WebKit is USE(BIGINT32)=0); a number seed truncates toward zero; a
// missing/other seed is 0. Per-algorithm width narrowing happens in bun_hash.
std::uint64_t hash_seed(JSC::JSGlobalObject* global, JSC::JSValue v) {
    if (!v) return 0;
    if (v.isHeapBigInt()) return v.toBigUInt64(global);  // exported ToBigUInt64, no throw for a bigint
    if (v.isNumber()) return static_cast<std::uint64_t>(v.asNumber());
    return 0;
}

JSC::EncodedJSValue encode_hash_result(JSC::JSGlobalObject* global, mbun::crypto::HashResult r) {
    if (r.is64) return JSC::JSValue::encode(JSC::JSBigInt::createFrom(global, r.value));
    return JSC::JSValue::encode(
        JSC::jsNumber(static_cast<double>(static_cast<std::uint32_t>(r.value))));
}

// Bun.hash(...) itself is wyhash → BigInt.
JSC_DEFINE_HOST_FUNCTION(bun_hash_default_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::uint64_t seed{hash_seed(global, native_arg(frame, 1))};
    return with_hash_bytes(global, native_arg(frame, 0), [&](std::span<const std::uint8_t> bytes) {
        return JSC::JSValue::encode(
            JSC::JSBigInt::createFrom(global, mbun::crypto::bun_hash_default(bytes, seed)));
    });
}

#define MBUN_HASH_HOST(FNNAME, ALGO)                                                            \
    JSC_DEFINE_HOST_FUNCTION(bun_hash_##FNNAME##_host,                                          \
                             (JSC::JSGlobalObject * global, JSC::CallFrame * frame)) {          \
        std::uint64_t seed{hash_seed(global, native_arg(frame, 1))};                                    \
        return with_hash_bytes(                                                                 \
            global, native_arg(frame, 0), [&](std::span<const std::uint8_t> bytes) {            \
                return encode_hash_result(                                                      \
                    global, mbun::crypto::bun_hash(mbun::crypto::HashAlgo::ALGO, seed, bytes)); \
            });                                                                                 \
    }

MBUN_HASH_HOST(wyhash, Wyhash)
MBUN_HASH_HOST(adler32, Adler32)
MBUN_HASH_HOST(crc32, Crc32)
MBUN_HASH_HOST(cityHash32, CityHash32)
MBUN_HASH_HOST(cityHash64, CityHash64)
MBUN_HASH_HOST(xxHash32, XxHash32)
MBUN_HASH_HOST(xxHash64, XxHash64)
MBUN_HASH_HOST(xxHash3, XxHash3)
MBUN_HASH_HOST(murmur32v2, Murmur32v2)
MBUN_HASH_HOST(murmur32v3, Murmur32v3)
MBUN_HASH_HOST(murmur64v2, Murmur64v2)
MBUN_HASH_HOST(rapidhash, Rapidhash)
#undef MBUN_HASH_HOST

// bun:internal-for-testing xxHash3ForTesting(input, seed?) — the full u64 seed
// XXH3_64bits_withSeed, exposed so the reference vectors can pin every length
// branch (internal-for-testing.ts:20). Unlike Bun.hash.xxHash3 it keeps all 64
// seed bits and validates the seed type: undefined → 0, a non-number/bigint
// throws (ref bun HashObject seed coercion).
JSC_DEFINE_HOST_FUNCTION(xxhash3_for_testing_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::JSValue seedV{native_arg(frame, 1)};
    std::uint64_t seed{0};
    if (seedV && !seedV.isUndefined()) {
        if (seedV.isHeapBigInt()) {
            seed = seedV.toBigUInt64(global);
        } else if (seedV.isNumber()) {
            seed = static_cast<std::uint64_t>(seedV.asNumber());
        } else {
            return throw_plain_error(global, "seed must be a number or bigint");
        }
    }
    return with_hash_bytes(global, native_arg(frame, 0), [&](std::span<const std::uint8_t> bytes) {
        return JSC::JSValue::encode(
            JSC::JSBigInt::createFrom(global, mbun::crypto::XxHash3::hash(seed, bytes)));
    });
}

// ── Bun.CryptoHasher + Bun.SHA1/SHA224/.../MD5 (mbun.crypto digest family) ───
//
// Backed by mbun.crypto::CryptoHasher (md5/sha1/sha224/sha256/sha384/sha512/
// sha512-256, with dash/slash/underscore-insensitive aliasing). Each instance
// owns its running digest state in the C API private slot; update/digest/copy
// are native host functions installed per instance (call-direct, no C API lock
// churn), mirroring the Bun.Glob wiring. DEFERRED(S2): HMAC keying (2nd ctor
// arg) — an unsupported algorithm or a supplied key throws rather than silently
// degrade.

// Binding-level hasher that unifies the mbun.crypto backends Bun.CryptoHasher can
// front: the CryptoHasher digest family (md5/sha1/sha2*), the Keccak sponge
// (sha3-*/shake*), and BLAKE2b/2s. All backends are value-copyable so copy() is a
// plain snapshot (mirrors CryptoHasher.rs copy before an EVP_DigestFinal), plus
// the standalone MD4 / RIPEMD-160 streaming classes.
struct NodeDigest {
    enum class Kind : std::uint8_t { Crypto, Keccak, Blake2b, Blake2s, Md4, Ripemd160 };
    Kind kind{Kind::Crypto};
    std::size_t out_len{0};
    std::optional<mbun::crypto::CryptoHasher> crypto;
    std::optional<mbun::crypto::Keccak> keccak;
    std::optional<mbun::crypto::Blake2b> blake2b;
    std::optional<mbun::crypto::Blake2s> blake2s;
    std::optional<mbun::crypto::Md4> md4;
    std::optional<mbun::crypto::Ripemd160> ripemd160;

    void update(std::span<const std::uint8_t> data) {
        switch (kind) {
        case Kind::Crypto: crypto->update(data); break;
        case Kind::Keccak: keccak->update(data); break;
        case Kind::Blake2b: blake2b->update(data); break;
        case Kind::Blake2s: blake2s->update(data); break;
        case Kind::Md4: md4->update(data); break;
        case Kind::Ripemd160: ripemd160->update(data); break;
        }
    }
    std::vector<std::uint8_t> digest() {
        if (kind == Kind::Crypto) { return crypto->digest(); }
        std::vector<std::uint8_t> out(out_len);
        switch (kind) {
        case Kind::Keccak: keccak->squeeze(out.data(), out_len); break;
        case Kind::Blake2b: blake2b->final_(out.data()); break;
        case Kind::Blake2s: blake2s->final_(out.data()); break;
        case Kind::Md4: md4->final_(out.data()); break;
        case Kind::Ripemd160: ripemd160->final_(out.data()); break;
        default: break;
        }
        return out;
    }
    std::size_t digest_length() const { return out_len; }
    NodeDigest copy() const { return *this; }
};

// Resolve a Bun.CryptoHasher algorithm name to a NodeDigest. nullopt = unsupported.
inline std::optional<NodeDigest> node_digest_by_name(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (char ch : name) {
        if (ch == '-' || ch == '_' || ch == '/' || ch == '.') { continue; }
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (auto algo{mbun::crypto::digest_algo_from(key)}) {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Crypto;
        d.crypto.emplace(*algo);
        d.out_len = d.crypto->digest_length();
        return d;
    }
    const auto keccak{[](std::size_t rate, std::uint8_t suffix, std::size_t out) {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Keccak;
        d.keccak.emplace(rate, suffix);
        d.out_len = out;
        return d;
    }};
    if (key == "sha3224") { return keccak(200 - 2 * 28, 0x06, 28); }
    if (key == "sha3256") { return keccak(200 - 2 * 32, 0x06, 32); }
    if (key == "sha3384") { return keccak(200 - 2 * 48, 0x06, 48); }
    if (key == "sha3512") { return keccak(200 - 2 * 64, 0x06, 64); }
    if (key == "shake128") { return keccak(168, 0x1f, 16); }
    if (key == "shake256") { return keccak(136, 0x1f, 32); }
    if (key == "blake2b512" || key == "blake2b256") {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Blake2b;
        d.out_len = key == "blake2b512" ? 64 : 32;
        d.blake2b.emplace(d.out_len);
        return d;
    }
    if (key == "blake2s256") {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Blake2s;
        d.out_len = 32;
        d.blake2s.emplace(32);
        return d;
    }
    if (key == "md4") {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Md4;
        d.out_len = mbun::crypto::Md4::DIGEST_LENGTH;
        d.md4.emplace();
        return d;
    }
    if (key == "ripemd160" || key == "rmd160") {
        NodeDigest d;
        d.kind = NodeDigest::Kind::Ripemd160;
        d.out_len = mbun::crypto::Ripemd160::DIGEST_LENGTH;
        d.ripemd160.emplace();
        return d;
    }
    return std::nullopt;
}

// Wrap a fixed DigestAlgo (Bun.SHA1 … Bun.MD5 classes) as a NodeDigest.
inline NodeDigest node_digest_from_algo(mbun::crypto::DigestAlgo algo) {
    NodeDigest d;
    d.kind = NodeDigest::Kind::Crypto;
    d.crypto.emplace(algo);
    d.out_len = d.crypto->digest_length();
    return d;
}

struct HasherState {
    NodeDigest hasher;
    std::string name;      // spelling used in messages + the `.algorithm` prop
    bool digested{false};
    // Bun.SHA*/MD5 (StaticCryptoHasher) latch after digest(): a second digest /
    // update / copy throws "already digested". Bun.CryptoHasher does NOT — its
    // final_ resets the context, so it can be reused and copied afterwards.
    // ref: src/runtime/crypto/CryptoHasher.rs (digested flag lives only on
    // StaticCryptoHasher).
    bool sticky{true};
};

JSClassRef gCryptoHasherClass{nullptr};

void crypto_hasher_finalize(JSObjectRef obj) {
    delete static_cast<HasherState*>(JSObjectGetPrivate(obj));
}

// Throw a real Error whose `.message` is `msg`, so expect().toThrow(/re/) (which
// matches against the message) behaves like bun's.
JSC::EncodedJSValue hasher_throw(JSC::JSGlobalObject* global, const std::string& msg) {
    return throw_plain_error(global, msg);
}

// Decode a hasher input into a byte span for `fn`. Returns false for missing/
// null/undefined (the caller throws). String → UTF-8 view; TypedArray/Buffer →
// backing store; a bare ArrayBuffer uses its full byte span; anything else is
// stringified (Blob support is DEFERRED — the consistency tests only compare
// like-against-like so still pass).
template <class F>
bool with_hasher_bytes(JSC::JSGlobalObject* global, JSC::JSValue v, F&& fn) {
    if (!v || v.isUndefinedOrNull()) { return false; }
    if (v.isCell()) {
        if (auto* view{WTF::dynamicDowncast<JSC::JSArrayBufferView>(v.asCell())}) {
            const std::uint8_t* p{view->isDetached() ? nullptr
                                                      : static_cast<const std::uint8_t*>(view->vector())};
            fn(std::span<const std::uint8_t>{p, (p != nullptr) ? view->byteLength() : 0});
            return true;
        }
        if (auto* wrapper{WTF::dynamicDowncast<JSC::JSArrayBuffer>(v.asCell())}) {
            JSC::ArrayBuffer* buffer{wrapper->impl()};
            if (buffer == nullptr || buffer->isDetached()) {
                fn(std::span<const std::uint8_t>{});
            } else {
                fn(buffer->span());
            }
            return true;
        }
    }
    JsStrArg s{global, v};
    std::string_view sv{s.view()};
    fn(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()});
    return true;
}

// Materialize raw digest bytes as a Uint8Array (Bun.CryptoHasher#digest() with
// no/"buffer" encoding). hex/base64 never carry embedded NULs so those go the
// make_string route in the callers.
JSC::JSValue make_u8array(JSC::JSGlobalObject* global, std::span<const std::uint8_t> bytes) {
    JSContextRef ctx{toRef(global)};
    JSObjectRef arr{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, bytes.size(), nullptr)};
    if (arr != nullptr && !bytes.empty()) {
        if (void* p{JSObjectGetTypedArrayBytesPtr(ctx, arr, nullptr)}) {
            std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(p));
        }
    }
    return toJS(global, static_cast<JSValueRef>(arr));
}

// Re-wrap a Uint8Array as a node Buffer via the JS `Buffer.from` (public C API
// only, safe inside a host callback). Bun.CryptoHasher#digest() and
// CryptoHasher.hash() hand back a Buffer, not a bare Uint8Array — only the
// fixed-algorithm Bun.SHA*/MD5 classes return a plain Uint8Array. ref:
// src/runtime/crypto/CryptoHasher.rs digest_to_bytes (ArrayBuffer::create_buffer)
// vs StaticCryptoHasher::digest_to_bytes (create_uint8_array).
JSC::JSValue as_buffer(JSC::JSGlobalObject* global, JSC::JSValue u8) {
    JSContextRef ctx{toRef(global)};
    JSValueRef bufV{get_prop(ctx, JSContextGetGlobalObject(ctx), "Buffer")};
    if (bufV != nullptr && JSValueIsObject(ctx, bufV)) {
        if (JSObjectRef bufObj{JSValueToObject(ctx, bufV, nullptr)}) {
            JSValueRef fromV{get_prop(ctx, bufObj, "from")};
            if (fromV != nullptr && JSValueIsObject(ctx, fromV)) {
                JSObjectRef fromFn{JSValueToObject(ctx, fromV, nullptr)};
                if (fromFn != nullptr && JSObjectIsFunction(ctx, fromFn)) {
                    JSValueRef arg{toRef(global, u8)};
                    if (JSValueRef r{JSObjectCallAsFunction(ctx, fromFn, bufObj, 1, &arg, nullptr)}) {
                        return toJS(global, r);
                    }
                }
            }
        }
    }
    return u8;
}

// Encode a finalized digest per an optional encoding argument (string → hex/
// base64/base64url; missing/"buffer"/other → Uint8Array, or a Buffer when
// `bufferOut`).
JSC::JSValue encode_digest(JSC::JSGlobalObject* global, std::span<const std::uint8_t> bytes,
                           JSC::JSValue encArg, bool bufferOut = false) {
    if (encArg && encArg.isString()) {
        JsStrArg e{global, encArg};
        if (auto enc{mbun::crypto::encoding_from(e.view())};
            enc && *enc != mbun::crypto::Encoding::Buffer) {
            return toJS(global, make_string(toRef(global), mbun::crypto::encode(bytes, *enc)));
        }
    }
    // Output-buffer form: digest(TypedArray) / SHA256.hash(input, TypedArray) copies
    // the raw digest into the supplied view and returns it (bun parity, ref
    // src/runtime/crypto/CryptoHasher.rs digest → writeToHashBuffer).
    if (encArg && encArg.isCell()) {
        if (auto* view{WTF::dynamicDowncast<JSC::JSArrayBufferView>(encArg.asCell())}) {
            if (!view->isDetached() && view->vector() != nullptr) {
                const std::size_t n{std::min(bytes.size(), static_cast<std::size_t>(view->byteLength()))};
                std::memcpy(view->vector(), bytes.data(), n);
            }
            return encArg;
        }
    }
    JSC::JSValue u8{make_u8array(global, bytes)};
    return bufferOut ? as_buffer(global, u8) : u8;
}

HasherState* hasher_state(JSC::JSValue thisV) {
    if (!thisV.isObject()) { return nullptr; }
    return static_cast<HasherState*>(JSObjectGetPrivate(toRef(JSC::asObject(thisV))));
}

JSObjectRef make_hasher_object(JSC::JSGlobalObject* global, JSClassRef cls,
                               NodeDigest h, std::string name, bool sticky = true);

JSC_DEFINE_HOST_FUNCTION(crypto_hasher_update_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    HasherState* st{hasher_state(frame->thisValue())};
    if (st == nullptr) { return hasher_throw(global, "CryptoHasher.update called on a non-CryptoHasher"); }
    if (st->digested) {
        return hasher_throw(global,
                            st->name + " hasher already digested, create a new instance to update");
    }
    bool ok{with_hasher_bytes(global, native_arg(frame, 0),
                              [&](std::span<const std::uint8_t> b) { st->hasher.update(b); })};
    if (!ok) {
        return hasher_throw(global,
                            "The \"data\" argument must be of type string or an instance of "
                            "Buffer, TypedArray, DataView, or ArrayBuffer.");
    }
    return JSC::JSValue::encode(frame->thisValue());  // chainable (returns this)
}

JSC_DEFINE_HOST_FUNCTION(crypto_hasher_digest_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    HasherState* st{hasher_state(frame->thisValue())};
    if (st == nullptr) { return hasher_throw(global, "CryptoHasher.digest called on a non-CryptoHasher"); }
    if (st->digested) {
        return hasher_throw(
            global, st->name + " hasher already digested, create a new instance to digest again");
    }
    auto bytes{st->hasher.digest()};
    if (st->sticky) {
        st->digested = true;
    } else if (auto fresh{node_digest_by_name(st->name)}) {
        // Bun.CryptoHasher's final_ resets the context: rebuild the same
        // algorithm so update()/digest()/copy() keep working afterwards.
        st->hasher = std::move(*fresh);
    }
    return JSC::JSValue::encode(encode_digest(global, bytes, native_arg(frame, 0), !st->sticky));
}

JSC_DEFINE_HOST_FUNCTION(crypto_hasher_copy_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    HasherState* st{hasher_state(frame->thisValue())};
    if (st == nullptr) { return hasher_throw(global, "CryptoHasher.copy called on a non-CryptoHasher"); }
    if (st->digested) {
        return hasher_throw(global,
                            st->name + " hasher already digested, create a new instance to copy");
    }
    JSObjectRef obj{
        make_hasher_object(global, gCryptoHasherClass, st->hasher.copy(), st->name, st->sticky)};
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

JSObjectRef make_hasher_object(JSC::JSGlobalObject* global, JSClassRef cls,
                               NodeDigest h, std::string name, bool sticky) {
    JSContextRef ctx{toRef(global)};
    auto* st{new HasherState{std::move(h), std::move(name), false, sticky}};
    JSObjectRef obj{JSObjectMake(ctx, cls, st)};
    set_native_fn(ctx, obj, "update", 1, crypto_hasher_update_host);
    set_native_fn(ctx, obj, "digest", 1, crypto_hasher_digest_host);
    set_native_fn(ctx, obj, "copy", 0, crypto_hasher_copy_host);
    // `.algorithm` / `.byteLength` are plain data props (bun's throw-after-digest
    // getter semantics are HMAC-only and DEFERRED).
    set_prop(ctx, obj, "algorithm", make_string(ctx, st->name));
    set_prop(ctx, obj, "byteLength",
             JSValueMakeNumber(ctx, static_cast<double>(st->hasher.digest_length())));
    return obj;
}

// new Bun.CryptoHasher(algorithm, key?) — key present ⇒ HMAC ⇒ DEFERRED (throws).
JSObjectRef crypto_hasher_construct_cb(JSContextRef ctx, JSObjectRef, std::size_t argc,
                                       const JSValueRef args[], JSValueRef* exc) {
    JSC::JSGlobalObject* global{toJS(ctx)};
    // JSC invokes C API constructor callbacks under JSLock::DropAllLocks
    // (APICallbackFunction::constructImpl); re-lock so the direct JSC-internal
    // work below (set_native_fn's JSFunction::create) runs with the VM's
    // atomStringTable installed (see set_fn in common.inc for the full story).
    JSC::JSLockHolder locker{global};
    const auto fail{[&](const std::string& msg) -> JSObjectRef {
        JSValueRef m{make_string(ctx, msg)};
        if (exc != nullptr) { *exc = JSObjectMakeError(ctx, 1, &m, nullptr); }
        return JSObjectMake(ctx, nullptr, nullptr);
    }};
    std::string algoName{
        (argc > 0 && !JSValueIsUndefined(ctx, args[0]) && !JSValueIsNull(ctx, args[0]))
            ? val_to_string(ctx, args[0])
            : std::string{"sha256"}};
    auto h{node_digest_by_name(algoName)};
    if (!h) { return fail("Unsupported algorithm \"" + algoName + "\""); }
    if (argc > 1 && !JSValueIsUndefined(ctx, args[1]) && !JSValueIsNull(ctx, args[1])) {
        return fail("HMAC keying is not yet supported");  // DEFERRED(S2)
    }
    return make_hasher_object(global, gCryptoHasherClass, std::move(*h), std::move(algoName), false);
}

// Bun.CryptoHasher.hash(algorithm, data, encoding?) — one-shot static.
JSC_DEFINE_HOST_FUNCTION(crypto_hasher_static_hash_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JsStrArg algoArg{global, native_arg(frame, 0)};
    std::string algoName{algoArg.view()};
    auto h{node_digest_by_name(algoName)};
    if (!h) { return hasher_throw(global, "Unsupported algorithm \"" + algoName + "\""); }
    bool ok{with_hasher_bytes(global, native_arg(frame, 1),
                              [&](std::span<const std::uint8_t> b) { h->update(b); })};
    if (!ok) {
        return hasher_throw(global,
                            "The \"data\" argument must be of type string or an instance of "
                            "Buffer, TypedArray, DataView, or ArrayBuffer.");
    }
    auto bytes{h->digest()};
    return JSC::JSValue::encode(encode_digest(global, bytes, native_arg(frame, 2), true));
}

// Fixed-algorithm classes Bun.SHA1/SHA224/SHA256/SHA384/SHA512/SHA512_256/MD5/MD4.
// Each gets its own JSClass so the constructor's `.name` is the class name (used
// verbatim in the "already digested" messages the tests assert on). The backend is
// a NodeDigest so the classes that are not in the CryptoHasher DigestAlgo family
// (MD4 — bun routes it through BoringSSL's EVP_md4) can join the same shape.
#define MBUN_SHA_CLASS(CLS, MAKE_DIGEST)                                                         \
    JSClassRef g##CLS##Class{nullptr};                                                           \
    JSObjectRef CLS##_construct_cb(JSContextRef ctx, JSObjectRef, std::size_t, const JSValueRef[],\
                                   JSValueRef*) {                                                 \
        JSC::JSGlobalObject* global{toJS(ctx)};                                                  \
        JSC::JSLockHolder locker{global}; /* ctor cbs run under DropAllLocks */                  \
        return make_hasher_object(global, g##CLS##Class, (MAKE_DIGEST), #CLS);                   \
    }                                                                                            \
    JSC_DEFINE_HOST_FUNCTION(CLS##_static_hash_host,                                             \
                             (JSC::JSGlobalObject * global, JSC::CallFrame * frame)) {           \
        NodeDigest h{MAKE_DIGEST};                                                               \
        bool ok{with_hasher_bytes(global, native_arg(frame, 0),                                  \
                                  [&](std::span<const std::uint8_t> b) { h.update(b); })};        \
        if (!ok) {                                                                                \
            return hasher_throw(global, "The \"data\" argument must be of type string or an "     \
                                        "instance of Buffer, TypedArray, DataView, or ArrayBuffer."); \
        }                                                                                        \
        auto bytes{h.digest()};                                                                  \
        return JSC::JSValue::encode(encode_digest(global, bytes, native_arg(frame, 1)));         \
    }
#define MBUN_ALGO(NAME) node_digest_from_algo(mbun::crypto::DigestAlgo::NAME)

MBUN_SHA_CLASS(SHA1, MBUN_ALGO(Sha1))
MBUN_SHA_CLASS(SHA224, MBUN_ALGO(Sha224))
MBUN_SHA_CLASS(SHA256, MBUN_ALGO(Sha256))
MBUN_SHA_CLASS(SHA384, MBUN_ALGO(Sha384))
MBUN_SHA_CLASS(SHA512, MBUN_ALGO(Sha512))
MBUN_SHA_CLASS(SHA512_256, MBUN_ALGO(Sha512_256))
MBUN_SHA_CLASS(MD5, MBUN_ALGO(Md5))
MBUN_SHA_CLASS(MD4, *node_digest_by_name("md4"))
#undef MBUN_ALGO
#undef MBUN_SHA_CLASS

// ── Bun.which (mbun.which PATH executable lookup) ────────────────────────────

// Real filesystem probes backing mbun::which::Fs. On POSIX, is_executable_file_
// path mirrors bun's bun_sys::is_executable_file_path: the path must stat as a
// *regular* file (directories are rejected — the test chmods a dir 755 and
// expects null) carrying any execute bit. exists_os_path is the Windows arm
// (DEFERRED there) and only needs plain existence.
mbun::which::Fs make_which_fs_() {
    mbun::which::Fs fs;
    fs.is_executable_file_path = [](std::string_view path) -> bool {
#if defined(_WIN32)
        (void)path;
        return false;
#else
        std::string p{path};
        struct ::stat st{};
        if (::stat(p.c_str(), &st) != 0) return false;
        if (!S_ISREG(st.st_mode)) return false;
        return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
#endif
    };
    fs.exists_os_path = [](std::string_view path) -> bool {
#if defined(_WIN32)
        std::string p{path};
        struct ::stat st{};
        return ::stat(p.c_str(), &st) == 0;
#else
        (void)path;
        return false;
#endif
    };
    return fs;
}

// Bun.which(bin, { PATH?, cwd? }) → absolute path string or null. Matches bun:
// bare names resolve through $PATH only (never the cwd); a slash-bearing name is
// resolved relative to cwd; an over-long bin throws "bin path is too long".
JSValueRef bun_which_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exception) {
    if (argc == 0 || JSValueIsUndefined(ctx, args[0]) || JSValueIsNull(ctx, args[0])) {
        return JSValueMakeNull(ctx);
    }
    std::string bin{val_to_string(ctx, args[0])};

    // bun throws when the requested bin path exceeds the platform PATH_MAX.
    if (bin.size() > mbun::which::MAX_PATH_BYTES) {
        if (exception != nullptr) {
            JSValueRef m{make_string(ctx, "bin path is too long")};
            *exception = JSObjectMakeError(ctx, 1, &m, nullptr);
        }
        return JSValueMakeNull(ctx);
    }

    // Options: explicit PATH / cwd override the process defaults.
    std::string pathVar;
    std::string cwd;
    bool havePath{false};
    if (argc > 1 && JSValueIsObject(ctx, args[1])) {
        JSObjectRef opts{JSValueToObject(ctx, args[1], nullptr)};
        JSValueRef p{get_prop(ctx, opts, "PATH")};
        if (p != nullptr && JSValueIsString(ctx, p)) {
            pathVar = val_to_string(ctx, p);
            havePath = true;
        }
        JSValueRef c{get_prop(ctx, opts, "cwd")};
        if (c != nullptr && JSValueIsString(ctx, c)) {
            cwd = val_to_string(ctx, c);
        }
    }
    if (!havePath) {
        // Bun's default comes from its VM dotenv/env map. In mbun that map is
        // represented by the live JS process.env object, so read it on every
        // call: .env additions and later JS mutations must both be visible.
        bool runtimeEnvAvailable{false};
        JSObjectRef global{JSContextGetGlobalObject(ctx)};
        JSValueRef processValue{get_prop(ctx, global, "process")};
        if (processValue != nullptr && JSValueIsObject(ctx, processValue)) {
            JSObjectRef process{JSValueToObject(ctx, processValue, nullptr)};
            JSValueRef envValue{get_prop(ctx, process, "env")};
            if (envValue != nullptr && JSValueIsObject(ctx, envValue)) {
                runtimeEnvAvailable = true;
                JSObjectRef env{JSValueToObject(ctx, envValue, nullptr)};
                JSValueRef pathValue{get_prop(ctx, env, "PATH")};
                if (pathValue != nullptr && JSValueIsString(ctx, pathValue)) {
                    pathVar = val_to_string(ctx, pathValue);
                    havePath = true;
                }
            }
        }
        // Defensive fallback for embedding contexts that replace/remove the
        // process object or process.env. A present runtime env with no PATH is
        // an intentionally empty search path and must not expose native PATH.
        if (!runtimeEnvAvailable) {
            if (const char* nativePath{std::getenv("PATH")}) pathVar = nativePath;
        }
    }
    if (cwd.empty()) {
        std::error_code ec;
        cwd = std::filesystem::current_path(ec).string();
    }

#if defined(_WIN32)
    constexpr mbun::which::Platform kPlatform{mbun::which::Platform::Win32};
#else
    constexpr mbun::which::Platform kPlatform{mbun::which::Platform::Posix};
#endif
    const mbun::which::Fs fs{make_which_fs_()};
    if (auto found{mbun::which::which(fs, pathVar, cwd, bin, kPlatform)}) {
        return make_string(ctx, *found);
    }
    return JSValueMakeNull(ctx);
}

// ── Bun.stringWidth ─────────────────────────────────────────────────────────

// Option lookup with bun's semantics: own properties are found anywhere on the
// explicit prototype chain, but the shared Object.prototype is NOT consulted —
// polluting Object.prototype.countAnsiEscapeCodes must not change results.
// Walks own enumerable names per level via the C API (cold path: options only).
static JSC::JSValue string_width_opt(JSC::JSGlobalObject* global, JSC::JSValue optsVal,
                                     const char* name) {
    if (!optsVal || !optsVal.isObject()) return {};
    // bun getIfPropertyExistsPrototypePollutionMitigation (ObjectBindings.cpp:1-93):
    // walk the chain probing each level's OWN slot; the first owning level wins,
    // and the shared Object.prototype is never consulted — so polluting
    // Object.prototype.countAnsiEscapeCodes cannot change results.
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    JSC::Identifier id{JSC::Identifier::fromString(vm, WTF::String::fromUTF8(name))};
    JSC::JSObject* object{JSC::asObject(optsVal)};
    JSC::JSObject* objectPrototype{global->objectPrototype()};
    while (true) {
        JSC::PropertySlot slot{object, JSC::PropertySlot::InternalMethodType::Get};
        bool has{object->methodTable()->getOwnPropertySlot(object, global, id, slot)};
        RETURN_IF_EXCEPTION(scope, {});
        if (has) { JSC::JSValue v{slot.getValue(global, id)}; RETURN_IF_EXCEPTION(scope, {}); return v; }
        JSC::JSValue proto{object->getPrototypeDirect()};
        if (!proto.isObject()) return {};
        object = JSC::asObject(proto);
        if (object == objectPrototype) return {};  // never consult shared Object.prototype
    }
}

JSC_DEFINE_HOST_FUNCTION(string_width_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::VM& vm{global->vm()};
    auto throwScope{DECLARE_THROW_SCOPE(vm)};
    JSC::JSValue value{native_arg(frame, 0)};
    // bun: missing/undefined/null → 0; other non-strings coerce via toString
    // (exceptions — e.g. Symbol's TypeError or a throwing toString — propagate).
    if (!value || value.isUndefinedOrNull()) return JSC::JSValue::encode(JSC::jsNumber(0));
    JSC::JSString* string{value.isString() ? JSC::asString(value) : value.toString(global)};
    RETURN_IF_EXCEPTION(throwScope, JSC::encodedJSValue());

    mbun::core::strings::StringWidthOptions opts{};
    JSC::JSValue optsVal{native_arg(frame, 1)};
    if (optsVal && optsVal.isObject()) {
        JSC::JSValue cae{string_width_opt(global, optsVal, "countAnsiEscapeCodes")};
        if (cae && !cae.isUndefined()) opts.count_ansi_escape_codes = cae.toBoolean(global);
        JSC::JSValue ain{string_width_opt(global, optsVal, "ambiguousIsNarrow")};
        if (ain && !ain.isUndefined()) opts.ambiguous_is_narrow = ain.toBoolean(global);
    }

    auto scope{string->view(global)};
    WTF::StringView view{scope};
    std::size_t width{};
    if (!view.is8Bit()) {
        const auto chars{view.span16()};
        width = mbun::core::strings::string_width(
            std::span<const char16_t>{chars.data(), chars.size()}, opts);
    } else {
        const auto bytes{view.span8()};
        width = mbun::core::strings::string_width(
            std::span<const unsigned char>{bytes.data(), bytes.size()}, opts);
    }
    return JSC::JSValue::encode(JSC::jsNumber(static_cast<double>(width)));
}

// ref: bun src/jsc/modules/BunJSCModule.h:527 jsFunctionCallerSourceOrigin —
// callFrame->callerSourceOrigin(vm), the URL/path of the calling frame's source.
JSC_DEFINE_HOST_FUNCTION(caller_source_origin_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::VM& vm{global->vm()};
    const JSC::SourceOrigin origin{frame->callerSourceOrigin(vm)};
    if (origin.isNull()) return JSC::JSValue::encode(JSC::jsEmptyString(vm));
    WTF::String s{origin.string()};
    // mbun evaluates module wrappers with a bare absolute path as sourceURL;
    // bun returns a URL (BunJSCModule.h:527) — file-URL-ify bare paths so
    // consumers can Bun.fileURLToPath() it. Must stay native: a JS wrapper
    // would itself become the "caller" (its origin is the builtins blob).
    if (s.startsWith('/')) s = WTF::makeString("file://"_s, s);
    return JSC::JSValue::encode(JSC::jsString(vm, s));
}

// ref: bun src/jsc/modules/BunJSCModule.h:931 functionEstimateDirectMemoryUsageOf
// (exported as bun:jsc estimateShallowMemoryUsageOf) — the cell's own estimated
// size, i.e. JSC's estimatedSizeInBytes(). A non-cell (number/bool/…) is 0.
// Native by necessity: the C API exposes no per-cell size.
JSC_DEFINE_HOST_FUNCTION(estimate_shallow_memory_usage_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::VM& vm{global->vm()};
    JSC::JSValue value{frame->argument(0)};
    if (!value || !value.isCell()) return JSC::JSValue::encode(JSC::jsNumber(0));
    JSC::EnsureStillAliveScope alive{value};
    return JSC::JSValue::encode(
        JSC::jsNumber(static_cast<double>(alive.value().asCell()->estimatedSizeInBytes(vm))));
}

// ── Bun.peek: synchronously read a settled promise (blueprint src/js/builtins/
// Peek.ts — $isPromise && status ? settledValue : value). Must be native: JSC's
// C API has no way to read a promise's internal state without .then (which would
// mark a rejection handled). status()/result() read the internal slots directly.
static JSC::JSPromise* mbun_as_promise(JSC::JSValue v) {
    if (!v.isObject()) return nullptr;
    JSC::JSObject* obj{v.getObject()};
    if (obj == nullptr || !obj->inherits<JSC::JSPromise>()) return nullptr;
    return static_cast<JSC::JSPromise*>(obj);
}
JSC_DEFINE_HOST_FUNCTION(peek_status_host,
                         (JSC::JSGlobalObject*, JSC::CallFrame* frame)) {
    JSC::JSPromise* p{mbun_as_promise(frame->argument(0))};
    if (p == nullptr) return JSC::JSValue::encode(JSC::jsNumber(-1));  // not a promise
    return JSC::JSValue::encode(JSC::jsNumber(static_cast<int>(p->status())));  // 0 pend / 1 ful / 2 rej
}
JSC_DEFINE_HOST_FUNCTION(peek_result_host,
                         (JSC::JSGlobalObject*, JSC::CallFrame* frame)) {
    JSC::JSValue v{frame->argument(0)};
    JSC::JSPromise* p{mbun_as_promise(v)};
    if (p == nullptr) return JSC::JSValue::encode(v);
    return JSC::JSValue::encode(p->result());
}

// ── bun:internal-for-testing string representation introspection ───────────

// ref: bun src/jsc/bindings/JSCTestingHelpers.cpp jsFunctionIsUTF16String.
// This observes JSC's actual backing representation after rope materialization;
// code-point content alone cannot distinguish an 8-bit string from a 16-bit one.
JSC_DEFINE_HOST_FUNCTION(is_utf16_string_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    JSC::JSValue value{native_arg(frame, 0)};
    JSC::JSString* jsString{value && value.isString() ? JSC::asString(value) : nullptr};
    if (jsString == nullptr) {
        JSC::throwTypeError(global, scope, WTF::String::fromLatin1("Expected a string"));
        return JSC::encodedJSValue();
    }

    WTF::String string{jsString->value(global)};
    RETURN_IF_EXCEPTION(scope, JSC::encodedJSValue());
    return JSC::JSValue::encode(JSC::jsBoolean(!string.is8Bit()));
}

// ── Bun.stripANSI (native core.strings; identity when nothing was stripped) ──

JSValueRef strip_ansi_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
    JsStrArg s{ctx, argc > 0 ? args[0] : nullptr};
    std::string out{mbun::core::strings::strip_ansi(s.view())};
    if (argc > 0 && out == s.view() && JSValueIsString(ctx, args[0])) return args[0];
    return make_string(ctx, out);
}

// ── shellInternals (bun:internal-for-testing) — mbun.shell parser bridge ─────

JSValueRef shell_parse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    JsStrArg src{ctx, argc > 0 ? args[0] : nullptr};
    double n{argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0};
    return make_string(ctx, mbun::shell::parse_json(src.view(), static_cast<std::uint32_t>(n)));
}

JSValueRef shell_parse_errors_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    JsStrArg src{ctx, argc > 0 ? args[0] : nullptr};
    double n{argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0};
    return make_string(ctx, mbun::shell::parse_errors(src.view(), static_cast<std::uint32_t>(n)));
}

// ── Bun.TOML.parse → real JS object tree ────────────────────────────────────

// Deepest table/array nesting Bun.TOML.parse materialises into JS. TOML's own
// array/inline-table parse is depth-capped (toml.cppm MAX_DEPTH), but DOTTED
// KEYS and headers are parsed ITERATIVELY — `a.a.a.…= 1` builds a Value tree as
// deep as the key has segments with no parse-time limit, and the recursive
// conversion below then runs the native stack out (SIGSEGV, whole process).
// bun surfaces that as a catchable RangeError instead.
// ref: compat/bun/test/js/bun/toml/toml.test.ts
//      "extremely deep dotted keys and headers throw instead of crashing"
// Well above the 1000-segment depth the suite requires to parse successfully
// ("deep dotted keys parse beyond the old 512-segment cap"), well under what
// either conversion can survive.
constexpr int kTomlMaxJsDepth{4096};

JSValueRef toml_to_js(JSContextRef ctx, const mbun::toml::Value& v, int depth, bool& tooDeep) {
    using Type = mbun::toml::Type;
    if (depth > kTomlMaxJsDepth) [[unlikely]] {
        tooDeep = true;
        return JSValueMakeUndefined(ctx);
    }
    switch (v.type()) {
    case Type::Table: {
        JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
        for (const auto& [k, child] : v.table().entries) {
            JSValueRef cv{toml_to_js(ctx, child, depth + 1, tooDeep)};
            if (tooDeep) return o;
            JSStringRef key{JSStringCreateWithUTF8CString(k.c_str())};
            JSObjectSetProperty(ctx, o, key, cv, kJSPropertyAttributeNone, nullptr);
            JSStringRelease(key);
        }
        return o;
    }
    case Type::Array: {
        const auto& arr{v.array()};
        std::vector<JSValueRef> elems;
        elems.reserve(arr.size());
        for (const auto& e : arr) {
            elems.push_back(toml_to_js(ctx, e, depth + 1, tooDeep));
            if (tooDeep) return JSValueMakeUndefined(ctx);
        }
        return JSObjectMakeArray(ctx, elems.size(), elems.empty() ? nullptr : elems.data(), nullptr);
    }
    case Type::String:
    case Type::Datetime:
        // bun materialises datetimes as JS Date; we emit the RFC string. This is
        // checksum-neutral for the TOML suite (checksum = top-level key count) and
        // keeps object-construction cost comparable. Noted in the report.
        return make_string(ctx, v.as_string());
    case Type::Integer:
        return JSValueMakeNumber(ctx, static_cast<double>(v.as_integer()));
    case Type::Float:
        return JSValueMakeNumber(ctx, v.as_float());
    case Type::Boolean:
        return JSValueMakeBoolean(ctx, v.as_bool());
    }
    return JSValueMakeUndefined(ctx);
}

// Serialize a toml::Value tree to JSON text (single contiguous buffer). Returns
// false when the tree cannot round-trip through JSON (non-finite floats: inf /
// nan are valid TOML but not valid JSON) — caller falls back to the per-node
// C API construction. This is the hot path of Bun.TOML.parse: ONE C++ pass to
// build the text + ONE JSC LiteralParser pass to build the object tree beats
// thousands of per-node C API calls (JSStringCreateWithUTF8CString +
// JSObjectSetProperty full property machinery per key).
bool toml_json(const mbun::toml::Value& v, std::string& out, int depth, bool& tooDeep) {
    using Type = mbun::toml::Type;
    if (depth > kTomlMaxJsDepth) [[unlikely]] {
        tooDeep = true;
        return false;
    }
    static constexpr auto escape{[](std::string_view s, std::string& o) {
        o.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20U) {
                        constexpr char hex[]{"0123456789abcdef"};
                        o += "\\u00";
                        o.push_back(hex[c >> 4]);
                        o.push_back(hex[c & 0xF]);
                    } else {
                        o.push_back(static_cast<char>(c));
                    }
            }
        }
        o.push_back('"');
    }};
    switch (v.type()) {
    case Type::Table: {
        out.push_back('{');
        bool first{true};
        for (const auto& [k, child] : v.table().entries) {
            if (!first) out.push_back(',');
            first = false;
            escape(k, out);
            out.push_back(':');
            if (!toml_json(child, out, depth + 1, tooDeep)) return false;
        }
        out.push_back('}');
        return true;
    }
    case Type::Array: {
        out.push_back('[');
        bool first{true};
        for (const auto& e : v.array()) {
            if (!first) out.push_back(',');
            first = false;
            if (!toml_json(e, out, depth + 1, tooDeep)) return false;
        }
        out.push_back(']');
        return true;
    }
    case Type::String:
    case Type::Datetime:
        escape(v.as_string(), out);
        return true;
    case Type::Integer: {
        char buf[24];
        auto [p, _]{std::to_chars(buf, buf + sizeof buf, v.as_integer())};
        out.append(buf, p);
        return true;
    }
    case Type::Float: {
        const double d{v.as_float()};
        if (!std::isfinite(d)) return false;  // inf/nan: JSON cannot express
        char buf[32];
        auto [p, _]{std::to_chars(buf, buf + sizeof buf, d)};
        out.append(buf, p);
        return true;
    }
    case Type::Boolean:
        out.append(v.as_bool() ? "true" : "false");
        return true;
    }
    return false;
}

JSValueRef toml_parse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    JsStrArg src{ctx, argc > 0 ? args[0] : nullptr};
    auto r{mbun::toml::parse(src.view())};
    if (!r) {
        // bun: a parse failure is a `SyntaxError` instance whose message is
        // "TOML Parse error: <detail>"; the depth guard's overflow is a
        // `RangeError` ("Maximum call stack size exceeded") instead — the toml
        // module marks that case with a "RangeError: " message prefix.
        // ref: compat/bun/src/runtime/api/TOMLObject.rs:26-46
        const std::string& msg{r.error().message};
        constexpr std::string_view kOverflow{"RangeError: "};
        if (exc != nullptr) {
            *exc = msg.starts_with(kOverflow)
                       ? make_range_error(ctx, "Maximum call stack size exceeded")
                       : make_syntax_error(ctx, "TOML Parse error: " + msg);
        }
        return JSValueMakeUndefined(ctx);
    }
    // Fast path: serialize to JSON text and let JSC's parser build the whole
    // object tree in one pass (public C API — stays inside the API entry / lock,
    // so GC allocation during the parse sees a consistent atomStringTable). This
    // beats thousands of per-node JSObjectSetProperty calls.
    std::string json;
    json.reserve(src.view().size() + src.view().size() / 2);
    bool tooDeep{false};
    if (toml_json(*r, json, 0, tooDeep)) {
        JSStringRef text{JSStringCreateWithUTF8CString(json.c_str())};
        JSValueRef val{JSValueMakeFromJSONString(ctx, text)};
        JSStringRelease(text);
        if (val != nullptr && !JSValueIsUndefined(ctx, val)) return val;
    }
    if (!tooDeep) {
        // Fallback (non-finite floats / invalid UTF-8): per-node construction.
        JSValueRef val{toml_to_js(ctx, *r, 0, tooDeep)};
        if (!tooDeep) return val;
    }
    // Nesting past kTomlMaxJsDepth: report the overflow the way bun does rather
    // than recursing the native stack to death.
    if (exc != nullptr) *exc = make_range_error(ctx, "Maximum call stack size exceeded");
    return JSValueMakeUndefined(ctx);
}

// ── Bun.Glob (JS class: instance stores pattern, .match(path) → bool) ─────────

JSClassRef gGlobClass{nullptr};

// Native host function for Glob.match (installed per-instance in the ctor, so it
// is called direct with no C API JSLock churn). `this` is the Glob instance; its
// pattern is read back through the C API private slot (JSObjectGetPrivate),
// keeping the C API class/finalizer machinery unchanged.
JSC_DEFINE_HOST_FUNCTION(glob_match_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};
    JSC::JSValue thisV{frame->thisValue()};
    if (!thisV.isObject()) return JSC::JSValue::encode(JSC::jsBoolean(false));
    auto* pat{static_cast<std::string*>(JSObjectGetPrivate(toRef(JSC::asObject(thisV))))};
    if (pat == nullptr) return JSC::JSValue::encode(JSC::jsBoolean(false));
    // ref: bun src/runtime/api/glob.zig Glob.match — a missing or non-string arg
    // throws rather than being coerced (validated before any matching happens).
    if (frame->argumentCount() == 0) {
        JSC::throwTypeError(
            global, scope,
            WTF::String::fromLatin1("Glob.matchString: expected 1 arguments, got 0"));
        return JSC::encodedJSValue();
    }
    JSC::JSValue strArg{frame->argument(0)};
    if (!strArg.isString()) {
        JSC::throwTypeError(
            global, scope,
            WTF::String::fromLatin1("Glob.matchString: first argument is not a string"));
        return JSC::encodedJSValue();
    }
    JsStrArg path{global, strArg};
    return JSC::JSValue::encode(JSC::jsBoolean(mbun::glob::match(*pat, path.view())));
}

// Glob.scanSync([cwd|{cwd}]) → array of matching file paths (relative, generic
// separators), by recursively walking the directory and matching each entry.
JSValueRef glob_scan_sync_cb(JSContextRef ctx, JSObjectRef, JSObjectRef thisObj, std::size_t argc,
                             const JSValueRef args[], JSValueRef*) {
    auto* pat{static_cast<std::string*>(JSObjectGetPrivate(thisObj))};
    if (pat == nullptr) return JSObjectMakeArray(ctx, 0, nullptr, nullptr);
    std::string cwd;
    bool followSymlinks{false};
    // bun Glob.scan option onlyFiles (glob walker only_files, default true) —
    // omit directories from results. node's fs.glob yields directories too, so
    // the fs.glob wrappers pass { onlyFiles: false }.
    bool onlyFiles{true};
    if (argc > 0) {
        if (JSValueIsString(ctx, args[0])) {
            cwd = val_to_string(ctx, args[0]);
        } else if (JSValueIsObject(ctx, args[0])) {
            JSObjectRef o{JSValueToObject(ctx, args[0], nullptr)};
            JSValueRef c{get_prop(ctx, o, "cwd")};
            if (JSValueIsString(ctx, c)) cwd = val_to_string(ctx, c);
            // bun Glob.scan option followSymlinks (glob walker
            // follow_directory_symlink; mbun glob.cppm:636 supports it).
            // STRICT own-property read: bun ignores inherited options — the
            // corpus pollutes Object.prototype.followSymlinks (enumerable) and
            // expects no effect, so for-in-style CopyPropertyNames (walks the
            // chain) is not usable here; JSC getDirect sees only own slots.
            {
                JSC::JSGlobalObject* g{toJS(ctx)};
                JSC::VM& vm{g->vm()};
                JSC::JSObject* jsObj{toJS(o)};
                if (jsObj != nullptr) {
                    JSC::JSValue own{jsObj->getDirect(
                        vm, JSC::Identifier::fromString(vm, "followSymlinks"_s))};
                    if (own && own.isBoolean()) followSymlinks = own.asBoolean();
                    JSC::JSValue onlyF{jsObj->getDirect(
                        vm, JSC::Identifier::fromString(vm, "onlyFiles"_s))};
                    if (onlyF && onlyF.isBoolean()) onlyFiles = onlyF.asBoolean();
                }
            }
        }
    }
    std::error_code ec;
    if (cwd.empty()) cwd = std::filesystem::current_path(ec).string();

    // onlyFiles=false → drive the full glob walker (Bun.Glob.scan semantics):
    // it emits matched directories AND the globstar-matches-parent-dir case
    // ("a/**" also yields "a") that the flat match-loop below cannot express.
    // The default onlyFiles=true keeps the original file-only fast path
    // untouched, so Bun.Glob.scanSync's default behaviour is unchanged.
    if (!onlyFiles) {
        mbun::glob::ScanOptions sopts;
        sopts.cwd = cwd;
        sopts.followSymlinks = followSymlinks;
        sopts.onlyFiles = false;
        const std::vector<std::string> rels{mbun::glob::scan(*pat, sopts)};
        std::vector<JSValueRef> matches;
        matches.reserve(rels.size());
        for (const std::string& rel : rels) matches.push_back(make_string(ctx, rel));
        return JSObjectMakeArray(ctx, matches.size(), matches.empty() ? nullptr : matches.data(),
                                 nullptr);
    }

    const std::filesystem::path base{cwd};
    std::vector<JSValueRef> matches;
    const auto dirOpts{followSymlinks
                           ? (std::filesystem::directory_options::skip_permission_denied |
                              std::filesystem::directory_options::follow_directory_symlink)
                           : std::filesystem::directory_options::skip_permission_denied};
    std::filesystem::recursive_directory_iterator it{base, dirOpts, ec};
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        // lexically_relative: fs::relative canonicalizes and would rewrite a
        // path that walks THROUGH a followed symlink to the target's realpath
        // (bun reports the logical walk path).
        const std::string rel{it->path().lexically_relative(base).generic_string()};
        if (!rel.empty() && mbun::glob::match(*pat, rel)) {
            matches.push_back(make_string(ctx, rel));
        }
    }
    return JSObjectMakeArray(ctx, matches.size(), matches.empty() ? nullptr : matches.data(),
                             nullptr);
}

JSObjectRef glob_construct_cb(JSContextRef ctx, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    // Ctor cbs run under DropAllLocks; re-lock before the JSC-internal work
    // below (JsStrArg rope resolve, set_native_fn) — see set_fn in common.inc.
    JSC::JSLockHolder locker{toJS(ctx)};
    // The pattern is stored on the instance, so an owned copy is unavoidable; the
    // zero-copy view still skips the C API malloc+transcode before the copy.
    JsStrArg pat{ctx, argc > 0 ? args[0] : nullptr};
    JSObjectRef obj{JSObjectMake(ctx, gGlobClass, new std::string(pat.view()))};
    // Native match on the instance (constructed once per pattern; not hot).
    set_native_fn(ctx, obj, "match", 1, glob_match_host);
    set_fn(ctx, obj, "scanSync", glob_scan_sync_cb);
    set_fn(ctx, obj, "scan", glob_scan_sync_cb);  // async iter over a sync array works with for-await
    return obj;
}

void glob_finalize(JSObjectRef obj) {
    delete static_cast<std::string*>(JSObjectGetPrivate(obj));
}

// No C API static functions — match is a native host function installed per
// instance in glob_construct_cb (only the terminator remains).
JSStaticFunction gGlobStaticFns[]{
    {nullptr, nullptr, 0},
};

// ── Bun.nanoseconds / Bun.file ──────────────────────────────────────────────

JSValueRef nanoseconds_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    auto ns{std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gStart)
                .count()};
    return JSValueMakeNumber(ctx, static_cast<double>(ns));
}

// Bun.file(x) → a shell carrying the file's raw bytes; the JS BunFile wrapper
// (builtins) turns it into a real Blob. The arg may be a URL object or a string;
// coercing through toString gives the href / path.
//
// Bytes (not a UTF-8 JSString) because bun's BunFile is a Blob over a byte store
// (bun src/runtime/blob: Blob::Store::File) — a JSString round-trip mangles every
// non-UTF-8 file (images/video) and silently truncated Bun.file("x.jpg").size to 0.
JSValueRef bunfile_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                      const JSValueRef args[], JSValueRef*) {
    std::string path{file_url_to_path(arg_string(ctx, argc, args, 0))};
    auto content{read_file(path)};
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    const std::string bytes{content.value_or(std::string{})};
    JSObjectRef arr{
        JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, bytes.size(), nullptr)};
    if (arr != nullptr && !bytes.empty()) {
        if (void* p{JSObjectGetTypedArrayBytesPtr(ctx, arr, nullptr)}) {
            std::copy(bytes.begin(), bytes.end(), static_cast<char*>(p));
        }
    }
    set_prop(ctx, o, "__bytes", static_cast<JSValueRef>(arr));
    // Bun.file is lazy: a missing/unreadable file yields no error at construction,
    // but text()/arrayBuffer()/bytes() must reject with the read errno (bun #26632).
    // Record the errno code so the JS BunFile wrapper can surface it on read.
    if (!content) {
        std::error_code ec;
        const char* code{std::filesystem::is_directory(std::filesystem::path{path}, ec)
                             ? "EISDIR"
                             : "ENOENT"};
        set_prop(ctx, o, "__ioerror", make_string(ctx, code));
    }
    return o;
}

// ── console.log / console.error ─────────────────────────────────────────────

void print_args(JSContextRef ctx, std::size_t argc, const JSValueRef args[], std::ostream& out) {
    std::string line;
    for (std::size_t i{0}; i < argc; ++i) {
        if (i != 0) line += ' ';
        line += val_to_string(ctx, args[i]);
    }
    line += '\n';
    out << line;
    out.flush();
}

JSValueRef console_log_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    print_args(ctx, argc, args, std::cout);
    return JSValueMakeUndefined(ctx);
}

JSValueRef console_error_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    print_args(ctx, argc, args, std::cerr);
    return JSValueMakeUndefined(ctx);
}

// ── __mbunHttpNative: process-wide http.maxHeaderSize ───────────────────────
// PORT-SOURCE: bun src/runtime/node/node_http_binding.rs:43-70
// (get/setMaxHTTPHeaderSize over bun_http::{max,set_max}_http_header_size).
JSValueRef http_get_max_header_size_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                       const JSValueRef[], JSValueRef*) {
    return JSValueMakeNumber(ctx, static_cast<double>(mbun::http::max_http_header_size()));
}

JSValueRef http_set_max_header_size_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                       const JSValueRef args[], JSValueRef* exception) {
    // bun: coerce_to_int64, then reject `num <= 0` as an invalid argument value.
    const double num{argc > 0 ? JSValueToNumber(ctx, args[0], nullptr) : 0};
    if (!(num >= 1) || !std::isfinite(num)) {
        if (exception != nullptr) {
            JSValueRef m{make_string(
                ctx, "The \"maxHeaderSize\" argument must be of type non-negative integer")};
            *exception = JSObjectMakeError(ctx, 1, &m, nullptr);
        }
        return JSValueMakeUndefined(ctx);
    }
    mbun::http::set_max_http_header_size(static_cast<std::size_t>(num));
    return JSValueMakeNumber(ctx, static_cast<double>(mbun::http::max_http_header_size()));
}

// `--user-agent <STR>` override, or undefined when the flag was absent.
// PORT-SOURCE: bun cli/Arguments.rs:1062-1066 → bun_http::OVERRIDDEN_DEFAULT_USER_AGENT
// (read by the fetch client when the request carries no User-Agent header).
JSValueRef http_get_user_agent_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                  const JSValueRef[], JSValueRef*) {
    if (gUserAgent.empty()) return JSValueMakeUndefined(ctx);
    return make_string(ctx, gUserAgent);
}
