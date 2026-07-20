// ── JSString ⇄ std::string helpers ──────────────────────────────────────────

// UTF-16 → UTF-8. We encode from the raw UTF-16 buffer rather than
// JSStringGetUTF8CString: that C-string API stops at the first embedded U+0000,
// silently truncating any JS string containing a NUL (test names, file contents,
// process env, the aggregated test-report body, …). Unpaired surrogates become
// U+FFFD, matching WebKit's replacement behaviour.
std::string to_utf8(JSStringRef jsStr) {
    const JSChar* u16{JSStringGetCharactersPtr(jsStr)};
    std::size_t len{JSStringGetLength(jsStr)};
    if (u16 == nullptr || len == 0) return {};
    std::string out;
    out.reserve(len + (len >> 1));
    for (std::size_t i{0}; i < len; ++i) {
        char32_t cp{u16[i]};
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
            if (i + 1 < len && u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (u16[i + 1] - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;  // unpaired high surrogate
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // unpaired low surrogate
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// JSValue → UTF-8 std::string (via the JS String coercion, same as bun's API
// boundary: an object stringifies through toString()/href).
std::string val_to_string(JSContextRef ctx, JSValueRef v) {
    if (v == nullptr) return {};
    JSValueRef exc{nullptr};
    JSStringRef s{JSValueToStringCopy(ctx, v, &exc)};
    if (s == nullptr) return {};
    std::string out{to_utf8(s)};
    JSStringRelease(s);
    return out;
}

std::string arg_string(JSContextRef ctx, std::size_t argc, const JSValueRef args[], std::size_t i) {
    if (i >= argc) return {};
    return val_to_string(ctx, args[i]);
}

JSValueRef make_string(JSContextRef ctx, std::string_view s) {
    std::string owned{s};
    JSStringRef js{JSStringCreateWithUTF8CString(owned.c_str())};
    JSValueRef v{JSValueMakeString(ctx, js)};
    JSStringRelease(js);
    return v;
}

// An error of a SPECIFIC native subclass — `JSObjectMakeError` only ever builds
// a plain `Error`, so `e instanceof SyntaxError` / `expect(fn).toThrow(RangeError)`
// fails on the values it makes. Built by calling the global constructor through
// the public C API only (no JSC internals), so it is safe inside a host
// callback. Falls back to a plain Error if the global was shadowed/removed.
JSValueRef make_named_error(JSContextRef ctx, const char* ctorName, std::string_view msg) {
    JSValueRef m{make_string(ctx, msg)};
    JSStringRef name{JSStringCreateWithUTF8CString(ctorName)};
    JSValueRef ctor{JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), name, nullptr)};
    JSStringRelease(name);
    if (ctor != nullptr && JSValueIsObject(ctx, ctor)) {
        JSObjectRef ctorObj{JSValueToObject(ctx, ctor, nullptr)};
        if (ctorObj != nullptr && JSObjectIsConstructor(ctx, ctorObj)) {
            JSObjectRef err{JSObjectCallAsConstructor(ctx, ctorObj, 1, &m, nullptr)};
            if (err != nullptr) return err;
        }
    }
    return JSObjectMakeError(ctx, 1, &m, nullptr);
}

JSValueRef make_range_error(JSContextRef ctx, std::string_view msg) {
    return make_named_error(ctx, "RangeError", msg);
}

JSValueRef make_syntax_error(JSContextRef ctx, std::string_view msg) {
    return make_named_error(ctx, "SyntaxError", msg);
}

// WebIDL ByteString → JS string: each raw byte 0xNN maps to U+00NN (Latin-1),
// never UTF-8 decoded. Used for HTTP header names/values (bytes ≥ 0x80 are
// valid; JSStringCreateWithUTF8CString would drop the whole string on invalid
// UTF-8, and truncates on embedded NUL). ref: WebKit HTTPHeaderMap / fetch spec.
JSValueRef make_latin1_string(JSContextRef ctx, std::string_view s) {
    std::vector<JSChar> chars;
    chars.reserve(s.size());
    for (unsigned char c : s) chars.push_back(static_cast<JSChar>(c));
    JSStringRef js{JSStringCreateWithCharacters(chars.data(), chars.size())};
    JSValueRef v{JSValueMakeString(ctx, js)};
    JSStringRelease(js);
    return v;
}

void set_prop(JSContextRef ctx, JSObjectRef obj, const char* name, JSValueRef v) {
    JSStringRef n{JSStringCreateWithUTF8CString(name)};
    JSObjectSetProperty(ctx, obj, n, v, kJSPropertyAttributeNone, nullptr);
    JSStringRelease(n);
}

JSValueRef get_prop(JSContextRef ctx, JSObjectRef obj, const char* name) {
    JSStringRef n{JSStringCreateWithUTF8CString(name)};
    JSValueRef v{JSObjectGetProperty(ctx, obj, n, nullptr)};
    JSStringRelease(n);
    return v;
}

// Install a host function with the C API callback signature but WITHOUT JSC's
// C-callback wrapper. JSObjectMakeFunctionWithCallback functions are invoked
// through APICallbackFunction::callImpl, which wraps every call in
// JSLock::DropAllLocks: the VM is unlocked and the thread's current
// atomStringTable is restored to the thread-default table for the whole
// callback. Plain C API calls inside the callback re-lock themselves, but any
// direct JSC-internal use (JsStrArg's rope resolve, set_native_fn's
// JSFunction::create, …) then runs unlocked — the first GC request in that
// window aborts on Heap::requestCollection's
// RELEASE_ASSERT(vm.atomStringTable() == current thread's), an intermittent
// SIGABRT paced by GC timing. A JSNativeStdFunction trampoline keeps the call
// inside the locked API entry (and skips the drop/re-grab churn entirely).
void set_fn(JSContextRef ctx, JSObjectRef obj, const char* name, JSObjectCallAsFunctionCallback cb) {
    JSC::JSGlobalObject* global{toJS(ctx)};
    JSC::JSNativeStdFunction* fn{JSC::JSNativeStdFunction::create(
        global->vm(), global, 0, WTF::String::fromUTF8(name),
        [cb](JSC::JSGlobalObject* g, JSC::CallFrame* frame) -> JSC::EncodedJSValue {
            JSC::VM& vm{g->vm()};
            auto scope{DECLARE_THROW_SCOPE(vm)};
            JSContextRef ctxRef{toRef(g)};
            JSObjectRef fnRef{toRef(frame->jsCallee())};
            JSObjectRef thisRef{
                toRef(JSC::asObject(frame->thisValue().toThis(g, JSC::ECMAMode::sloppy())))};
#if CPU(ADDRESS64)
            // JSValueRef is the encoded JSValue on 64-bit (APICast.h): reuse the
            // call frame's argument span in place, as JSC's own callImpl does.
            const std::span<JSC::JSValue> args{frame->argumentsSpan()};
            const auto* argv{std::bit_cast<const JSValueRef*>(args.data())};
            const std::size_t argc{args.size()};
#else
            WTF::Vector<JSValueRef, 16> argBuf(frame->argumentCount(), [&](std::size_t i) {
                return toRef(g, frame->uncheckedArgument(i));
            });
            const JSValueRef* argv{argBuf.span().data()};
            const std::size_t argc{argBuf.size()};
#endif
            JSValueRef exception{nullptr};
            JSValueRef result{cb(ctxRef, fnRef, thisRef, argc, argv, &exception)};
            if (exception != nullptr) {
                JSC::throwException(g, scope, toJS(g, exception));
                return JSC::JSValue::encode(JSC::jsUndefined());
            }
            if (result == nullptr) return JSC::JSValue::encode(JSC::jsUndefined());
            return JSC::JSValue::encode(toJS(g, result));
        })};
    set_prop(ctx, obj, name, toRef(fn));
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Percent-decode a file:// URL to a filesystem path (Node fileURLToPath): strip
// scheme + optional host, then %XX-decode. Returns nullopt for a malformed URL
// (embedded NUL, or a bare host with no path) so callers can report it.
std::optional<std::string> file_url_to_path_checked(std::string_view url) {
    constexpr std::string_view kPfx{"file://"};
    if (url.starts_with(kPfx)) {
        url.remove_prefix(kPfx.size());
        if (!url.empty() && url.front() != '/') {          // file://host/path → /path
            auto slash{url.find('/')};
            if (slash == std::string_view::npos) return std::nullopt;
            url.remove_prefix(slash);
        }
    }
    constexpr auto hex{[](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    }};
    std::string out;
    out.reserve(url.size());
    for (std::size_t i{0}; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size()) {
            int hi{hex(url[i + 1])}, lo{hex(url[i + 2])};
            if (hi >= 0 && lo >= 0) { out.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
        }
        out.push_back(url[i]);
    }
    if (out.find('\0') != std::string::npos) return std::nullopt;
    return out;
}

// Strip a leading file:// URL down to a filesystem path (file:///abs → /abs).
std::string file_url_to_path(std::string s) {
    if (auto p{file_url_to_path_checked(s)}) return *p;
    return s;
}

// Process start reference for Bun.nanoseconds() (monotonic; only deltas matter).
const std::chrono::steady_clock::time_point gStart{std::chrono::steady_clock::now()};

// Process argv exposed to scripts as process.argv / Bun.argv.
std::vector<std::string> gArgv{};

// `--no-env-file` / bunfig env=false: suppress default .env auto-loading.
bool gDisableEnvFiles{false};
