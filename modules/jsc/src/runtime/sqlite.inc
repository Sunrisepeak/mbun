// ── __mbunSqliteNative: real sqlite3 backend for bun:sqlite ─────────────────
// Powers the bun:sqlite Database/Statement JS classes (js_builtins.cppm layer).
// mbun.sqlite.native (global-module-fragment sqlite3.h) does the actual
// open/prepare/step/bind/exec; this bridge marshals params (a positional JS
// array — the JS layer resolves named $x/:x/@x to sqlite positions) and results
// across the JSC C API boundary. Values cross as native JS types: number/string/
// null/Uint8Array/boolean/bigint. Handles are integer ids into a process-global
// registry (single JS thread, same rationale as __mbunNetNative).

// Live Database objects keyed by integer handle. Database is move-only; the map
// owns it and drops (closing the sqlite3 handle) on erase.
std::unordered_map<int, mbun::sqlite::Database>& sqlite_dbs() {
    static std::unordered_map<int, mbun::sqlite::Database> gDbs;
    return gDbs;
}
int& sqlite_next_handle() {
    static int gNext{1};
    return gNext;
}

// Primary sqlite3 result-code number for a masked code name (extended codes are
// not recoverable here — the native layer masks rc to its primary code).
int sqlite_code_errno(const std::string& code) {
    static const std::unordered_map<std::string, int> kMap{
        {"SQLITE_OK", 0}, {"SQLITE_ERROR", 1}, {"SQLITE_INTERNAL", 2}, {"SQLITE_PERM", 3},
        {"SQLITE_ABORT", 4}, {"SQLITE_BUSY", 5}, {"SQLITE_LOCKED", 6}, {"SQLITE_NOMEM", 7},
        {"SQLITE_READONLY", 8}, {"SQLITE_INTERRUPT", 9}, {"SQLITE_IOERR", 10},
        {"SQLITE_CORRUPT", 11}, {"SQLITE_NOTFOUND", 12}, {"SQLITE_FULL", 13},
        {"SQLITE_CANTOPEN", 14}, {"SQLITE_PROTOCOL", 15}, {"SQLITE_SCHEMA", 17},
        {"SQLITE_TOOBIG", 18}, {"SQLITE_CONSTRAINT", 19}, {"SQLITE_MISMATCH", 20},
        {"SQLITE_MISUSE", 21}, {"SQLITE_AUTH", 23}, {"SQLITE_RANGE", 25}, {"SQLITE_NOTADB", 26}};
    auto it{kMap.find(code)};
    return it != kMap.end() ? it->second : 1;
}

// Throw a JS Error carrying .code/.errno/.byteOffset and name "SQLiteError"
// (bun's SQLiteError hasInstance hook matches on name, so `instanceof` works).
JSValueRef sqlite_throw(JSContextRef ctx, JSValueRef* exc, const std::string& msg,
                        const std::string& code) {
    if (exc != nullptr) {
        JSValueRef m{make_string(ctx, msg)};
        JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
        set_prop(ctx, err, "code", make_string(ctx, code));
        set_prop(ctx, err, "name", make_string(ctx, "SQLiteError"));
        set_prop(ctx, err, "errno", JSValueMakeNumber(ctx, sqlite_code_errno(code)));
        set_prop(ctx, err, "byteOffset", JSValueMakeNumber(ctx, -1));
        *exc = err;
    }
    return JSValueMakeUndefined(ctx);
}

// Read the bytes backing a typed-array view or ArrayBuffer into a blob.
std::optional<std::vector<std::byte>> sqlite_read_bytes(JSContextRef ctx, JSValueRef v) {
    JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, v, nullptr)};
    if (tt == kJSTypedArrayTypeNone) return std::nullopt;
    JSObjectRef obj{JSValueToObject(ctx, v, nullptr)};
    if (obj == nullptr) return std::nullopt;
    const std::byte* p{nullptr};
    std::size_t n{0};
    if (tt == kJSTypedArrayTypeArrayBuffer) {
        p = static_cast<const std::byte*>(JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
        n = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
    } else {
        // BytesPtr answers the BUFFER base, ByteLength is view-relative:
        // without the offset a subarray is read from the wrong bytes.
        p = static_cast<const std::byte*>(JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
        if (p != nullptr) p += JSObjectGetTypedArrayByteOffset(ctx, obj, nullptr);
        n = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
    }
    if (p == nullptr) return std::vector<std::byte>{};
    return std::vector<std::byte>{p, p + n};
}

// Convert one JS value to a native SqlValue. On a BigInt outside int64 range,
// reports an error so the caller can throw RangeError (bun parity).
std::expected<mbun::sqlite::SqlValue, std::string>
sqlite_js_to_value(JSContextRef ctx, JSValueRef v) {
    using mbun::sqlite::SqlValue;
    switch (JSValueGetType(ctx, v)) {
    case kJSTypeUndefined:
    case kJSTypeNull:
        return SqlValue{nullptr};
    case kJSTypeBoolean:
        return SqlValue{JSValueToBoolean(ctx, v)};
    case kJSTypeNumber: {
        double d{JSValueToNumber(ctx, v, nullptr)};
        if (std::isfinite(d) && std::trunc(d) == d
            && d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
            return SqlValue{static_cast<std::int64_t>(d)};
        }
        return SqlValue{d};
    }
    case kJSTypeString:
        return SqlValue{val_to_string(ctx, v)};
    case kJSTypeBigInt: {
        std::string s{val_to_string(ctx, v)};
        std::int64_t out{0};
        auto [ptr, ec]{std::from_chars(s.data(), s.data() + s.size(), out)};
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            return std::unexpected(std::string{"BigInt value is out of range"});
        }
        return SqlValue{out};
    }
    default:
        break;
    }
    if (auto bytes{sqlite_read_bytes(ctx, v)}) return SqlValue{std::move(*bytes)};
    return std::unexpected(std::string{"unsupported bind value type"});
}

// Native SqlValue → JS value (integers as Number per bun's default mode).
JSValueRef sqlite_value_to_js(JSContextRef ctx, const mbun::sqlite::SqlValue& v) {
    using mbun::sqlite::SQLValueKind;
    switch (v.kind()) {
    case SQLValueKind::Null:
        return JSValueMakeNull(ctx);
    case SQLValueKind::Integer:
        return JSValueMakeNumber(ctx, static_cast<double>(std::get<std::int64_t>(v.value())));
    case SQLValueKind::Real:
        return JSValueMakeNumber(ctx, std::get<double>(v.value()));
    case SQLValueKind::Boolean:
        return JSValueMakeNumber(ctx, std::get<bool>(v.value()) ? 1.0 : 0.0);
    case SQLValueKind::Text:
        return make_string(ctx, std::get<std::string>(v.value()));
    case SQLValueKind::Blob: {
        const auto& b{std::get<std::vector<std::byte>>(v.value())};
        JSObjectRef ta{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, b.size(), nullptr)};
        if (!b.empty()) {
            void* p{JSObjectGetTypedArrayBytesPtr(ctx, ta, nullptr)};
            if (p != nullptr) std::memcpy(p, b.data(), b.size());
        }
        return ta;
    }
    }
    return JSValueMakeNull(ctx);
}

const char* sqlite_type_name(mbun::sqlite::SQLValueKind kind) {
    using mbun::sqlite::SQLValueKind;
    switch (kind) {
    case SQLValueKind::Integer:
    case SQLValueKind::Boolean: return "INTEGER";
    case SQLValueKind::Real:    return "FLOAT";
    case SQLValueKind::Text:    return "TEXT";
    case SQLValueKind::Blob:    return "BLOB";
    case SQLValueKind::Null:    return "NULL";
    }
    return "NULL";
}

// open(filename, readonly, create) → integer handle.
JSValueRef sqliten_open_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string filename{arg_string(ctx, argc, args, 0)};
    mbun::sqlite::DatabaseOptions opts{};
    opts.readonly = argc > 1 && JSValueToBoolean(ctx, args[1]);
    opts.create = argc > 2 ? JSValueToBoolean(ctx, args[2]) : true;

    // PERMISSION GATE. `new Database(path)` is a filesystem open, and with bun's
    // default flags (readwrite|create) it is also a filesystem WRITE that creates
    // the file if it is missing — so under `--permission` this was an unrestricted
    // arbitrary-file-create/write primitive while fs.writeFileSync and Bun.write
    // were both being denied. An in-memory database touches no path and stays free.
    const bool inMemory{filename.empty() || filename == ":memory:"
                        || filename.starts_with("file::memory:")
                        || filename.find("mode=memory") != std::string::npos};
    if (!inMemory) {
        if (permission_deny_fs(ctx, exc, filename, false)) return JSValueMakeUndefined(ctx);
        if (!opts.readonly && permission_deny_fs(ctx, exc, filename, true)) {
            return JSValueMakeUndefined(ctx);
        }
    }
    // The gate above can only see the path it was handed. `ATTACH DATABASE '<p>'`
    // opens a second file from inside a SQL string, so a connection created under a
    // partial fs grant must not be able to attach at all.
    opts.no_attach = gPermission.enabled()
                     && !(gPermission.is_granted(mbun::permission::Scope::FileSystemRead)
                          && gPermission.is_granted(mbun::permission::Scope::FileSystemWrite));

    auto db{mbun::sqlite::open(std::move(filename), opts)};
    if (!db) return sqlite_throw(ctx, exc, db.error().message, db.error().code);
    int handle{sqlite_next_handle()++};
    sqlite_dbs().emplace(handle, std::move(*db));
    return JSValueMakeNumber(ctx, static_cast<double>(handle));
}

// close(handle) — idempotent; drops the Database (closes the sqlite3 handle).
JSValueRef sqliten_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    sqlite_dbs().erase(handle);
    return JSValueMakeUndefined(ctx);
}

// inTransaction(handle) → bool.
JSValueRef sqliten_in_transaction_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                     const JSValueRef args[], JSValueRef*) {
    int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto it{sqlite_dbs().find(handle)};
    bool active{it != sqlite_dbs().end() && it->second.in_transaction()};
    return JSValueMakeBoolean(ctx, active);
}

// Collect a positional params vector from a JS array argument.
std::expected<std::vector<mbun::sqlite::SqlValue>, std::string>
sqlite_collect_params(JSContextRef ctx, JSValueRef paramsV) {
    std::vector<mbun::sqlite::SqlValue> vals;
    if (paramsV == nullptr || JSValueIsUndefined(ctx, paramsV) || JSValueIsNull(ctx, paramsV)) {
        return vals;
    }
    JSObjectRef arr{JSValueToObject(ctx, paramsV, nullptr)};
    if (arr == nullptr) return vals;
    double len{JSValueToNumber(ctx, get_prop(ctx, arr, "length"), nullptr)};
    auto n{static_cast<unsigned>(std::max(0.0, len))};
    vals.reserve(n);
    for (unsigned i{0}; i < n; ++i) {
        JSValueRef el{JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)};
        auto v{sqlite_js_to_value(ctx, el)};
        if (!v) return std::unexpected(v.error());
        vals.push_back(std::move(*v));
    }
    return vals;
}

// run(handle, sql, paramsArray?) → { columns, types, values, changes,
// lastInsertRowid }. values is an array of row-arrays; the JS layer shapes it
// into object/values/raw rows.
// Statement.toString() -> SQL with bound parameters rendered in (sqlite3_expanded_sql).
JSValueRef sqliten_expanded_sql_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
    int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto it{sqlite_dbs().find(handle)};
    if (it == sqlite_dbs().end()) {
        return sqlite_throw(ctx, exc, "Cannot use a closed database", "SQLITE_MISUSE");
    }
    std::string sql{arg_string(ctx, argc, args, 1)};
    auto params{sqlite_collect_params(ctx, argc > 2 ? args[2] : nullptr)};
    if (!params) return sqlite_throw(ctx, exc, params.error(), "SQLITE_MISMATCH");
    auto res{it->second.expanded_sql(sql, *params)};
    if (!res) return sqlite_throw(ctx, exc, res.error().message, res.error().code);
    return make_string(ctx, *res);
}

JSValueRef sqliten_run_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto it{sqlite_dbs().find(handle)};
    if (it == sqlite_dbs().end()) {
        return sqlite_throw(ctx, exc, "Database has closed", "SQLITE_MISUSE");
    }
    std::string sql{arg_string(ctx, argc, args, 1)};
    auto params{sqlite_collect_params(ctx, argc > 2 ? args[2] : nullptr)};
    if (!params) {
        if (params.error().starts_with("BigInt")) {
            if (exc != nullptr) {
                JSValueRef m{make_string(ctx, params.error())};
                *exc = JSObjectMakeError(ctx, 1, &m, nullptr);
            }
            return JSValueMakeUndefined(ctx);
        }
        return sqlite_throw(ctx, exc, params.error(), "SQLITE_MISMATCH");
    }

    auto res{it->second.prepare(sql).bind(std::move(*params)).execute()};
    if (!res) return sqlite_throw(ctx, exc, res.error().message, res.error().code);
    const auto& r{*res};

    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    JSObjectRef columns{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    JSObjectRef types{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    JSObjectRef declaredTypes{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    JSObjectRef values{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    // Column names come from the prepared statement, not from a row: a SELECT
    // matching zero rows still has columns. Mirrors bun's initializeColumnNames,
    // which reads sqlite3_column_name without stepping (JSSQLStatement.cpp).
    for (unsigned i{0}; i < r.columns.size(); ++i) {
        JSObjectSetPropertyAtIndex(ctx, columns, i, make_string(ctx, r.columns[i]), nullptr);
    }
    // Declared types from sqlite3_column_decltype; empty string => no declared
    // type (expression/function), surfaced to JS as null (JSSQLStatement.cpp:2741).
    for (unsigned i{0}; i < r.declared_types.size(); ++i) {
        JSValueRef dt{r.declared_types[i].empty() ? JSValueMakeNull(ctx)
                                                  : make_string(ctx, r.declared_types[i])};
        JSObjectSetPropertyAtIndex(ctx, declaredTypes, i, dt, nullptr);
    }
    if (!r.rows.empty()) {
        const auto& first{r.rows.front().values()};
        for (unsigned i{0}; i < first.size(); ++i) {
            JSObjectSetPropertyAtIndex(ctx, types, i, make_string(ctx, sqlite_type_name(first[i].kind())), nullptr);
        }
        for (unsigned ri{0}; ri < r.rows.size(); ++ri) {
            const auto& row{r.rows[ri].values()};
            JSObjectRef rowArr{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
            for (unsigned ci{0}; ci < row.size(); ++ci) {
                JSObjectSetPropertyAtIndex(ctx, rowArr, ci, sqlite_value_to_js(ctx, row[ci]), nullptr);
            }
            JSObjectSetPropertyAtIndex(ctx, values, ri, rowArr, nullptr);
        }
    }
    set_prop(ctx, out, "columns", columns);
    set_prop(ctx, out, "types", types);
    set_prop(ctx, out, "declaredTypes", declaredTypes);
    set_prop(ctx, out, "values", values);
    set_prop(ctx, out, "changes", JSValueMakeNumber(ctx, static_cast<double>(r.changes)));
    set_prop(ctx, out, "lastInsertRowid",
             JSValueMakeNumber(ctx, static_cast<double>(r.last_insert_rowid)));
    return out;
}
