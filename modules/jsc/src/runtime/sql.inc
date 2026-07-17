// Bun.sql (postgres) native bridge: exposes the pure-logic mbun.postgres wire
// codec (frontend encoders + backend decoder + column value conversion) to JS
// as __mbunPostgresNative. The socket/TLS transport + event-loop pump is
// DEFERRED(S-net); this file is codec-only so the query-construction and
// result-parsing (injected bytes) paths are exercisable offline. The JS-facing
// Bun.sql API shape lives in the sql builtin partition.
//
// Blueprint: modules/postgres (wire.cppm / protocol.cppm), itself aligned to
// bun-ref src/sql/postgres/PostgresProtocol.rs and the PostgreSQL v3 wire spec.

// Read the bytes backing a Uint8Array / ArrayBuffer argument (empty on null).
std::vector<std::byte> psql_read_bytes(JSContextRef ctx, JSValueRef v) {
    if (v == nullptr || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) return {};
    JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, v, nullptr)};
    if (tt == kJSTypedArrayTypeNone) return {};
    JSObjectRef obj{JSValueToObject(ctx, v, nullptr)};
    if (obj == nullptr) return {};
    const std::byte* p{nullptr};
    std::size_t n{0};
    if (tt == kJSTypedArrayTypeArrayBuffer) {
        p = static_cast<const std::byte*>(JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
    } else {
        p = static_cast<const std::byte*>(JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
    }
    if (p == nullptr) return {};
    return std::vector<std::byte>{p, p + n};
}

// Materialize a byte span as a fresh Uint8Array.
JSValueRef psql_make_bytes(JSContextRef ctx, std::span<const std::byte> bytes) {
    JSObjectRef ta{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, bytes.size(), nullptr)};
    if (ta != nullptr && !bytes.empty()) {
        if (void* p{JSObjectGetTypedArrayBytesPtr(ctx, ta, nullptr)}) {
            std::copy(bytes.begin(), bytes.end(), static_cast<std::byte*>(p));
        }
    }
    return ta;
}

// Collect a JS array of u32 (parameter type OIDs) — non-integers coerce via number.
std::vector<std::uint32_t> psql_read_u32_array(JSContextRef ctx, JSValueRef v) {
    std::vector<std::uint32_t> out;
    if (v == nullptr || !JSValueIsObject(ctx, v)) return out;
    JSObjectRef arr{JSValueToObject(ctx, v, nullptr)};
    if (arr == nullptr) return out;
    auto len{static_cast<unsigned>(std::max(0.0, JSValueToNumber(ctx, get_prop(ctx, arr, "length"), nullptr)))};
    out.reserve(len);
    for (unsigned i{0}; i < len; ++i) {
        out.push_back(static_cast<std::uint32_t>(JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr), nullptr)));
    }
    return out;
}

// ── frontend encoders (each returns a Uint8Array) ────────────────────────────

// encodeStartup({ user, database, ...extra }) — key/value string params.
JSValueRef psql_encode_startup_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef*) {
    std::vector<mbun::postgres::StartupParam> params;
    if (argc > 0 && JSValueIsObject(ctx, args[0])) {
        JSObjectRef obj{JSValueToObject(ctx, args[0], nullptr)};
        JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, obj)};
        std::size_t n{JSPropertyNameArrayGetCount(names)};
        for (std::size_t i{0}; i < n; ++i) {
            JSStringRef key{JSPropertyNameArrayGetNameAtIndex(names, i)};
            std::string k{to_utf8(key)};
            JSValueRef val{JSObjectGetProperty(ctx, obj, key, nullptr)};
            if (JSValueIsUndefined(ctx, val) || JSValueIsNull(ctx, val)) continue;
            JSStringRef vs{JSValueToStringCopy(ctx, val, nullptr)};
            params.push_back({std::move(k), vs != nullptr ? to_utf8(vs) : std::string{}});
            if (vs != nullptr) JSStringRelease(vs);
        }
        JSPropertyNameArrayRelease(names);
    }
    auto frame{mbun::postgres::encode_startup(params)};
    return psql_make_bytes(ctx, frame);
}

JSValueRef psql_encode_query_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    auto frame{mbun::postgres::encode_query(arg_string(ctx, argc, args, 0))};
    return psql_make_bytes(ctx, frame);
}

// encodeParse(name, query, [oid,...])
JSValueRef psql_encode_parse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    std::string name{arg_string(ctx, argc, args, 0)};
    std::string query{arg_string(ctx, argc, args, 1)};
    auto oids{psql_read_u32_array(ctx, argc > 2 ? args[2] : nullptr)};
    auto frame{mbun::postgres::encode_parse(name, query, oids)};
    return psql_make_bytes(ctx, frame);
}

// encodeBind(portal, statement, [Uint8Array|null,...]) — text-format params.
JSValueRef psql_encode_bind_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
    std::string portal{arg_string(ctx, argc, args, 0)};
    std::string statement{arg_string(ctx, argc, args, 1)};
    std::vector<mbun::postgres::BindValue> params;
    std::vector<std::vector<std::byte>> owned;  // keep spans alive
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef arr{JSValueToObject(ctx, args[2], nullptr)};
        auto len{static_cast<unsigned>(std::max(0.0, JSValueToNumber(ctx, get_prop(ctx, arr, "length"), nullptr)))};
        owned.reserve(len);
        for (unsigned i{0}; i < len; ++i) {
            JSValueRef el{JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)};
            if (JSValueIsNull(ctx, el) || JSValueIsUndefined(ctx, el)) {
                params.emplace_back(std::nullopt);
            } else {
                owned.push_back(psql_read_bytes(ctx, el));
                params.emplace_back(owned.back());
            }
        }
    }
    // text format for all (empty format lists mean "all text").
    auto frame{mbun::postgres::encode_bind(portal, statement, {}, params, {})};
    return psql_make_bytes(ctx, frame);
}

JSValueRef psql_encode_describe_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef*) {
    std::string kind{arg_string(ctx, argc, args, 0)};
    auto frame{mbun::postgres::encode_describe(kind.empty() ? 'S' : kind[0], arg_string(ctx, argc, args, 1))};
    return psql_make_bytes(ctx, frame);
}

JSValueRef psql_encode_execute_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef*) {
    std::string portal{arg_string(ctx, argc, args, 0)};
    auto maxRows{argc > 1 ? static_cast<std::int32_t>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    auto frame{mbun::postgres::encode_execute(portal, maxRows)};
    return psql_make_bytes(ctx, frame);
}

JSValueRef psql_encode_sync_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                               const JSValueRef[], JSValueRef*) {
    auto frame{mbun::postgres::encode_sync()};
    return psql_make_bytes(ctx, frame);
}
JSValueRef psql_encode_flush_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                const JSValueRef[], JSValueRef*) {
    auto frame{mbun::postgres::encode_flush()};
    return psql_make_bytes(ctx, frame);
}
JSValueRef psql_encode_terminate_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                    const JSValueRef[], JSValueRef*) {
    auto frame{mbun::postgres::encode_terminate()};
    return psql_make_bytes(ctx, frame);
}

// ── backend decoder ──────────────────────────────────────────────────────────

// Build the JS object for one decoded backend message.
JSObjectRef psql_message_to_js(JSContextRef ctx, const mbun::postgres::BackendMessage& m) {
    using namespace mbun::postgres;
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    char tag{static_cast<char>(m.tag)};
    set_prop(ctx, o, "tag", make_string(ctx, std::string_view{&tag, 1}));
    std::visit([&](const auto& p) {
        using T = std::remove_cvref_t<decltype(p)>;
        if constexpr (std::is_same_v<T, AuthenticationMsg>) {
            set_prop(ctx, o, "kind", JSValueMakeNumber(ctx, static_cast<double>(p.kind)));
            if (p.kind == AuthKind::MD5Password)
                set_prop(ctx, o, "md5Salt", psql_make_bytes(ctx, p.md5_salt));
            if (!p.sasl_mechanisms.empty()) {
                JSObjectRef arr{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
                for (unsigned i{0}; i < p.sasl_mechanisms.size(); ++i)
                    JSObjectSetPropertyAtIndex(ctx, arr, i, make_string(ctx, p.sasl_mechanisms[i]), nullptr);
                set_prop(ctx, o, "saslMechanisms", arr);
            }
            if (!p.data.empty()) set_prop(ctx, o, "data", psql_make_bytes(ctx, p.data));
        } else if constexpr (std::is_same_v<T, ParameterStatusMsg>) {
            set_prop(ctx, o, "name", make_string(ctx, p.name));
            set_prop(ctx, o, "value", make_string(ctx, p.value));
        } else if constexpr (std::is_same_v<T, BackendKeyDataMsg>) {
            set_prop(ctx, o, "processId", JSValueMakeNumber(ctx, p.process_id));
            set_prop(ctx, o, "secretKey", JSValueMakeNumber(ctx, p.secret_key));
        } else if constexpr (std::is_same_v<T, ReadyForQueryMsg>) {
            char s{p.status};
            set_prop(ctx, o, "status", make_string(ctx, std::string_view{&s, 1}));
        } else if constexpr (std::is_same_v<T, RowDescriptionMsg>) {
            JSObjectRef fields{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
            for (unsigned i{0}; i < p.fields.size(); ++i) {
                const auto& f{p.fields[i]};
                JSObjectRef fo{JSObjectMake(ctx, nullptr, nullptr)};
                set_prop(ctx, fo, "name", make_string(ctx, f.name));
                set_prop(ctx, fo, "tableOid", JSValueMakeNumber(ctx, f.table_oid));
                set_prop(ctx, fo, "columnIndex", JSValueMakeNumber(ctx, f.column_index));
                set_prop(ctx, fo, "typeOid", JSValueMakeNumber(ctx, f.type_oid));
                set_prop(ctx, fo, "typeSize", JSValueMakeNumber(ctx, f.type_size));
                set_prop(ctx, fo, "typeModifier", JSValueMakeNumber(ctx, f.type_modifier));
                set_prop(ctx, fo, "formatCode", JSValueMakeNumber(ctx, f.format_code));
                JSObjectSetPropertyAtIndex(ctx, fields, i, fo, nullptr);
            }
            set_prop(ctx, o, "fields", fields);
        } else if constexpr (std::is_same_v<T, DataRowMsg>) {
            JSObjectRef cols{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
            for (unsigned i{0}; i < p.columns.size(); ++i) {
                const auto& c{p.columns[i]};
                JSValueRef v{c.has_value() ? psql_make_bytes(ctx, *c) : JSValueMakeNull(ctx)};
                JSObjectSetPropertyAtIndex(ctx, cols, i, v, nullptr);
            }
            set_prop(ctx, o, "columns", cols);
        } else if constexpr (std::is_same_v<T, CommandCompleteMsg>) {
            set_prop(ctx, o, "command", make_string(ctx, p.tag));
        } else if constexpr (std::is_same_v<T, ParameterDescriptionMsg>) {
            JSObjectRef arr{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
            for (unsigned i{0}; i < p.type_oids.size(); ++i)
                JSObjectSetPropertyAtIndex(ctx, arr, i, JSValueMakeNumber(ctx, p.type_oids[i]), nullptr);
            set_prop(ctx, o, "typeOids", arr);
        } else if constexpr (std::is_same_v<T, NotificationResponseMsg>) {
            set_prop(ctx, o, "processId", JSValueMakeNumber(ctx, p.process_id));
            set_prop(ctx, o, "channel", make_string(ctx, p.channel));
            set_prop(ctx, o, "payload", make_string(ctx, p.payload));
        } else if constexpr (std::is_same_v<T, ErrorResponseMsg>) {
            JSObjectRef fo{JSObjectMake(ctx, nullptr, nullptr)};
            for (const auto& f : p.fields) {
                char code{f.code};
                std::string key{std::string_view{&code, 1}};
                set_prop(ctx, fo, key.c_str(), make_string(ctx, f.value));
            }
            set_prop(ctx, o, "fields", fo);
            set_prop(ctx, o, "severity", make_string(ctx, p.severity()));
            set_prop(ctx, o, "code", make_string(ctx, p.code_str()));
            set_prop(ctx, o, "message", make_string(ctx, p.message()));
        }
        // std::monostate (tag-only messages): tag alone is enough.
    }, m.payload);
    return o;
}

// decode(Uint8Array) → { messages: [...], consumed: n }. Drains as many whole
// messages as are buffered; `consumed` is how many bytes to drop before the
// next decode call (so a partial trailing message is retried after more bytes).
JSValueRef psql_decode_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    auto bytes{psql_read_bytes(ctx, argc > 0 ? args[0] : nullptr)};
    std::string_view buf{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    JSObjectRef arr{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    std::size_t consumed{0};
    unsigned idx{0};
    while (consumed < buf.size()) {
        auto r{mbun::postgres::decode_message(buf.substr(consumed))};
        if (r.status == mbun::postgres::DecodeStatus::Incomplete) break;
        if (r.status == mbun::postgres::DecodeStatus::Invalid) {
            // surface the invalid framing so the JS layer can raise a protocol error
            JSObjectRef bad{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, bad, "tag", make_string(ctx, "invalid"));
            JSObjectSetPropertyAtIndex(ctx, arr, idx++, bad, nullptr);
            break;
        }
        JSObjectSetPropertyAtIndex(ctx, arr, idx++, psql_message_to_js(ctx, r.message), nullptr);
        consumed += r.bytes_read;
    }
    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, out, "messages", arr);
    set_prop(ctx, out, "consumed", JSValueMakeNumber(ctx, static_cast<double>(consumed)));
    return out;
}

// decodeValue(typeOid, Uint8Array, formatCode) → bool | number | string | Uint8Array
// | null. int8 outside the safe-integer range is returned as a BigInt.
JSValueRef psql_decode_value_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    auto oid{argc > 0 ? static_cast<std::uint32_t>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto bytes{psql_read_bytes(ctx, argc > 1 ? args[1] : nullptr)};
    auto format{argc > 2 ? static_cast<std::int16_t>(JSValueToNumber(ctx, args[2], nullptr)) : 0};
    auto value{mbun::postgres::decode_value(static_cast<mbun::postgres::TypeId>(oid), bytes, format)};
    return std::visit([&](const auto& v) -> JSValueRef {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) return JSValueMakeNull(ctx);
        else if constexpr (std::is_same_v<T, bool>) return JSValueMakeBoolean(ctx, v);
        else if constexpr (std::is_same_v<T, std::int64_t>) {
            constexpr std::int64_t kSafe{9007199254740991};
            if (v <= kSafe && v >= -kSafe) return JSValueMakeNumber(ctx, static_cast<double>(v));
            // outside 2^53: build an exact BigInt via the global BigInt(string).
            JSObjectRef global{JSContextGetGlobalObject(ctx)};
            JSValueRef bigIntFn{get_prop(ctx, global, "BigInt")};
            if (JSValueIsObject(ctx, bigIntFn)) {
                JSObjectRef fn{JSValueToObject(ctx, bigIntFn, nullptr)};
                JSValueRef arg{make_string(ctx, std::to_string(v))};
                if (JSValueRef bi{JSObjectCallAsFunction(ctx, fn, nullptr, 1, &arg, nullptr)}) return bi;
            }
            return JSValueMakeNumber(ctx, static_cast<double>(v));
        } else if constexpr (std::is_same_v<T, double>) return JSValueMakeNumber(ctx, v);
        else if constexpr (std::is_same_v<T, std::string>) return make_string(ctx, v);
        else return psql_make_bytes(ctx, v);
    }, value);
}
