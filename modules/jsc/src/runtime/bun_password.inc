// runtime/bun_password.inc — Bun.password native backend (globalThis.__mbunPasswordNative).
//
// Raw compute for Bun.password.hash/verify. Argument validation, algorithm
// option parsing, and error-message shaping live in the JS layer
// (builtins/bun_password.cppm); this layer only extracts bytes and calls the
// pure-C++ mbun.crypto bcrypt / argon2 backends. bcrypt silently truncates
// passwords >72 bytes, so (matching Bun) they are SHA-512 pre-hashed first.
// ref: bun src/runtime/crypto/PasswordObject.rs + pwhash.rs.

// __mbunPasswordNative.hash(password, algorithm, cost, memoryCost, timeCost)
// → the crypt/PHC hash string. Trusts the JS layer to have validated params.
JSC_DEFINE_HOST_FUNCTION(bun_password_hash_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::vector<std::uint8_t> pw;
    const bool ok { with_hasher_bytes(global, native_arg(frame, 0),
                                      [&](std::span<const std::uint8_t> b) {
                                          pw.assign(b.begin(), b.end());
                                      }) };
    if (!ok) { return hasher_throw(global, "password must be a string or TypedArray"); }

    JsStrArg algoArg { global, native_arg(frame, 1) };
    const std::string algo { algoArg.view() };

    std::string out;
    if (algo == "bcrypt") {
        JSC::JSValue costV { native_arg(frame, 2) };
        const int cost { costV.isNumber() ? static_cast<int>(costV.asNumber()) : 10 };
        std::vector<std::uint8_t> key { pw };
        if (pw.size() > 72) {
            mbun::crypto::CryptoHasher h { mbun::crypto::DigestAlgo::Sha512 };
            h.update(pw);
            key = h.digest();
        }
        out = mbun::crypto::bcrypt::hash_random(key, static_cast<std::uint8_t>(cost));
    } else {
        mbun::crypto::argon2::Type type { mbun::crypto::argon2::Type::ID };
        if (algo == "argon2i") {
            type = mbun::crypto::argon2::Type::I;
        } else if (algo == "argon2d") {
            type = mbun::crypto::argon2::Type::D;
        }
        JSC::JSValue mV { native_arg(frame, 3) };
        JSC::JSValue tV { native_arg(frame, 4) };
        const std::uint32_t m { mV.isNumber() ? static_cast<std::uint32_t>(mV.asNumber()) : 65536 };
        const std::uint32_t t { tV.isNumber() ? static_cast<std::uint32_t>(tV.asNumber()) : 2 };
        out = mbun::crypto::argon2::hash_random(type, m, t, 1, pw);
    }
    return JSC::JSValue::encode(toJS(global, make_string(toRef(global), out)));
}

// __mbunPasswordNative.verify(password, hash, algorithm?) → status code:
//   1 = match, 0 = mismatch, 2 = WeakParameters, 3 = InvalidEncoding,
//   4 = UnsupportedAlgorithm. The JS layer turns non-{0,1} codes into throws.
JSC_DEFINE_HOST_FUNCTION(bun_password_verify_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::vector<std::uint8_t> pw;
    std::vector<std::uint8_t> hashBytes;
    const bool pok { with_hasher_bytes(global, native_arg(frame, 0),
                                       [&](std::span<const std::uint8_t> b) {
                                           pw.assign(b.begin(), b.end());
                                       }) };
    const bool hok { with_hasher_bytes(global, native_arg(frame, 1),
                                       [&](std::span<const std::uint8_t> b) {
                                           hashBytes.assign(b.begin(), b.end());
                                       }) };
    if (!pok || !hok) { return hasher_throw(global, "password and hash must be string or TypedArray"); }
    const std::string hashStr { hashBytes.begin(), hashBytes.end() };

    std::string algo;
    JSC::JSValue algoV { native_arg(frame, 2) };
    if (algoV && algoV.isString()) {
        JsStrArg a { global, algoV };
        algo = a.view();
    }

    bool isBcrypt { algo == "bcrypt" };
    bool isArgon { algo == "argon2i" || algo == "argon2d" || algo == "argon2id" };
    if (!isBcrypt && !isArgon) {
        if (hashStr.starts_with("$2")) {
            isBcrypt = true;
        } else if (hashStr.starts_with("$argon2")) {
            isArgon = true;
        }
    }

    int code { 4 };
    if (isBcrypt) {
        std::vector<std::uint8_t> key { pw };
        if (pw.size() > 72) {
            mbun::crypto::CryptoHasher h { mbun::crypto::DigestAlgo::Sha512 };
            h.update(pw);
            key = h.digest();
        }
        const auto r { mbun::crypto::bcrypt::verify(key, hashStr) };
        code = !r ? 3 : (*r ? 1 : 0);
    } else if (isArgon) {
        using VR = mbun::crypto::argon2::VerifyResult;
        switch (mbun::crypto::argon2::verify(pw, hashStr)) {
        case VR::Match: code = 1; break;
        case VR::Mismatch: code = 0; break;
        case VR::WeakParameters: code = 2; break;
        case VR::InvalidEncoding: code = 3; break;
        }
    }
    return JSC::JSValue::encode(JSC::jsNumber(code));
}
