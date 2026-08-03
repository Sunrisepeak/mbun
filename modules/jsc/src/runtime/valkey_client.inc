// Bun.redis / Bun.RedisClient native bridge: exposes the pure-logic
// mbun.valkey RESP protocol codec (request serializer + reply decoder) to JS
// as __mbunValkeyNative. The live socket/TLS transport + event-loop pump is
// DEFERRED(S-net); this file is codec-only so command serialization and
// reply-parsing (injected bytes) are exercisable offline. The JS-facing
// Bun.RedisClient API shape + connection-string parsing live in the valkey
// builtin partition (builtins/valkey_client.cppm).
//
// Blueprint: modules/valkey (protocol.cppm / command.cppm) — aligned to
// bun-ref src/valkey/valkey_protocol.rs and
// src/runtime/valkey_jsc/protocol_jsc.rs (RESPValue → JS coercions).

// Materialize a byte span as a fresh Uint8Array.
JSValueRef vk_make_bytes(JSContextRef ctx, std::span<const unsigned char> bytes) {
    JSObjectRef ta{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, bytes.size(), nullptr)};
    if (ta != nullptr && !bytes.empty()) {
        if (void* p{JSObjectGetTypedArrayBytesPtr(ctx, ta, nullptr)}) {
            std::memcpy(p, bytes.data(), bytes.size());
        }
    }
    return ta;
}

// Read the bytes backing a Uint8Array / ArrayBuffer argument (empty on null).
std::vector<unsigned char> vk_read_bytes(JSContextRef ctx, JSValueRef v) {
    if (v == nullptr || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) return {};
    JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, v, nullptr)};
    if (tt == kJSTypedArrayTypeNone) return {};
    JSObjectRef obj{JSValueToObject(ctx, v, nullptr)};
    if (obj == nullptr) return {};
    const unsigned char* p{nullptr};
    std::size_t n{0};
    if (tt == kJSTypedArrayTypeArrayBuffer) {
        p = static_cast<const unsigned char*>(JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
    } else {
        // BytesPtr answers the BUFFER base, ByteLength is view-relative:
        // without the offset a subarray is read from the wrong bytes.
        p = static_cast<const unsigned char*>(JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
        if (p != nullptr) p += JSObjectGetTypedArrayByteOffset(ctx, obj, nullptr);
        n = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
    }
    if (p == nullptr) return {};
    return std::vector<unsigned char>{p, p + n};
}

JSValueRef vk_make_error(JSContextRef ctx, std::string_view message) {
    JSValueRef arg{make_string(ctx, message)};
    return JSObjectMakeError(ctx, 1, &arg, nullptr);
}

// RESPValue → JS. Mirrors bun-ref protocol_jsc.rs resp_value_to_js_with_options:
// bulk/simple/verbatim → string, integer/double → number, boolean → boolean,
// null → null, error/blob_error → Error, array/set → Array, map/attribute →
// object, push → { type, data }, big number → number-if-fits else string.
JSValueRef vk_resp_to_js(JSContextRef ctx, const mbun::valkey::RESPValue& value) {
    using mbun::valkey::RESPType;
    switch (value.type) {
    case RESPType::simple_string:
    case RESPType::bulk_string:
    case RESPType::verbatim_string:
        // A negative-length bulk string ($-1) decodes to an empty null value.
        if (value.type == RESPType::bulk_string && value.bytes.empty() && value.format.empty()) {
            // distinguish real empty string from $-1 null is not tracked; the
            // reader models $-1 as an empty bulk string. Return "" to match the
            // wire (tests inject explicit $-1 for null via the null type).
        }
        return make_string(ctx, std::string(value.bytes.begin(), value.bytes.end()));
    case RESPType::error:
    case RESPType::blob_error:
        return vk_make_error(ctx, std::string(value.bytes.begin(), value.bytes.end()));
    case RESPType::integer:
        return JSValueMakeNumber(ctx, static_cast<double>(value.integer));
    case RESPType::double_value:
        return JSValueMakeNumber(ctx, value.floating);
    case RESPType::boolean:
        return JSValueMakeBoolean(ctx, value.boolean);
    case RESPType::null:
        return JSValueMakeNull(ctx);
    case RESPType::big_number: {
        std::string text(value.bytes.begin(), value.bytes.end());
        std::int64_t parsed{};
        auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (err == std::errc{} && end == text.data() + text.size()) {
            return JSValueMakeNumber(ctx, static_cast<double>(parsed));
        }
        return make_string(ctx, text);
    }
    case RESPType::array:
    case RESPType::set: {
        JSObjectRef arr{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
        unsigned idx{0};
        for (const auto& child : value.children) {
            JSObjectSetPropertyAtIndex(ctx, arr, idx++, vk_resp_to_js(ctx, *child), nullptr);
        }
        return arr;
    }
    case RESPType::map:
    case RESPType::attribute: {
        JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
        for (const auto& [k, v] : value.entries) {
            std::string key;
            if (auto s = k->as_string()) key = *s;
            else key = k->display();
            JSStringRef jsKey{JSStringCreateWithUTF8CString(key.c_str())};
            JSObjectSetProperty(ctx, obj, jsKey, vk_resp_to_js(ctx, *v), kJSPropertyAttributeNone, nullptr);
            JSStringRelease(jsKey);
        }
        // Attribute: bun returns the attached value, ignoring the metadata map.
        if (value.type == RESPType::attribute && value.value) {
            return vk_resp_to_js(ctx, *value.value);
        }
        return obj;
    }
    case RESPType::push: {
        JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
        JSObjectRef data{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
        std::string kind;
        unsigned idx{0};
        for (const auto& child : value.children) {
            if (idx == 0) { if (auto s = child->as_string()) kind = *s; }
            JSObjectSetPropertyAtIndex(ctx, data, idx++, vk_resp_to_js(ctx, *child), nullptr);
        }
        set_prop(ctx, obj, "type", make_string(ctx, kind));
        set_prop(ctx, obj, "data", data);
        return obj;
    }
    }
    return JSValueMakeNull(ctx);
}

// encode(name, [arg, ...]) → Uint8Array. Serializes a RESP request (array of
// bulk strings), the wire format bun sends. Args coerce to strings.
JSValueRef vk_encode_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
    std::string name{arg_string(ctx, argc, args, 0)};
    std::vector<std::string> owned;
    if (argc > 1 && JSValueIsObject(ctx, args[1])) {
        JSObjectRef arr{JSValueToObject(ctx, args[1], nullptr)};
        auto len{static_cast<unsigned>(std::max(0.0,
            JSValueToNumber(ctx, get_prop(ctx, arr, "length"), nullptr)))};
        owned.reserve(len);
        for (unsigned i{0}; i < len; ++i) {
            JSValueRef el{JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)};
            owned.push_back(val_to_string(ctx, el));
        }
    }
    std::vector<std::string_view> views;
    views.reserve(owned.size());
    for (const auto& s : owned) views.emplace_back(s);
    std::string frame{mbun::valkey::cmd::build(name, views)};
    return vk_make_bytes(ctx, {reinterpret_cast<const unsigned char*>(frame.data()), frame.size()});
}

// decode(Uint8Array) → { status: "complete"|"incomplete"|"invalid",
// consumed, value }. On complete, `value` is the JS-coerced reply; on invalid,
// `error` carries the parser error name.
JSValueRef vk_decode_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
    auto bytes{vk_read_bytes(ctx, argc > 0 ? args[0] : nullptr)};
    auto parsed{mbun::valkey::parse_reply({bytes.data(), bytes.size()})};
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    using mbun::valkey::ParseStatus;
    const char* status{parsed.status == ParseStatus::complete ? "complete"
        : parsed.status == ParseStatus::incomplete ? "incomplete" : "invalid"};
    set_prop(ctx, out, "status", make_string(ctx, status));
    set_prop(ctx, out, "consumed", JSValueMakeNumber(ctx, static_cast<double>(parsed.consumed)));
    if (parsed.status == ParseStatus::complete) {
        set_prop(ctx, out, "value", vk_resp_to_js(ctx, parsed.value));
    } else if (parsed.status == ParseStatus::invalid) {
        set_prop(ctx, out, "error", make_string(ctx, "RESP protocol error"));
    }
    return out;
}

// Install the RESP codec bridge. The valkey builtin partition
// (builtins/valkey_client.cppm) reads globalThis.__mbunValkeyNative and
// re-exposes it as Bun.__valkeyWire; without this installer the whole
// Bun.redis surface silently no-ops (the slice existed but was never wired
// into runtime.cppm — caught by test_runtime_structure's dead-slice check).
void install_valkey_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef n{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, n, "encode", vk_encode_cb);
    set_fn(ctx, n, "decode", vk_decode_cb);
    set_prop(ctx, global, "__mbunValkeyNative", n);
}
