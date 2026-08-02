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

// process.argv0 — node's snapshot of the ORIGINAL argv[0], exactly as the OS
// handed it over (a bare name when resolved through PATH, a path otherwise).
// It is deliberately NOT gArgv.front(): that slot carries the runtime's own
// display name ("mbun"/"bun"/"node") and is later rewritten to execPath by the
// JS layer, while argv0 must stay whatever invoked us — the corpus respawns the
// runtime through it (`spawn(process.argv0, …)`).
// ref: node lib/internal/bootstrap/node.js (process.argv0 = argv[0] snapshot).
std::string gArgv0{};

// process.execArgv — the runtime's own flags, which are NEVER part of argv.
// Empty for an ordinary `mbun run`; a `bun build --compile --compile-exec-argv=…`
// executable carries the flags that were baked into it.
// ref: bun src/runtime/node/node_process.rs (execArgv) and
// test/bundler/compile-argv.test.ts "CompileExecArgvNoLeak".
std::vector<std::string> gExecArgv{};

// The dialect this process serves — see runtime.cppm for what it is and who
// resolves it. Bun by default, so a binary nobody told anything to behaves
// exactly as it did before the dialect existed.
mbun::jsc::runtime::Dialect gDialect{mbun::jsc::runtime::Dialect::Bun};

// `--no-env-file` / bunfig env=false: suppress default .env auto-loading.
bool gDisableEnvFiles{false};

// `--env-file <path>` — explicit dotenv files. ref: bun dotenv/env_loader.rs:657
// `Loader::load`: a non-empty explicit list REPLACES the default-file scan
// entirely (so `--no-env-file --env-file x` still loads x), and the list is
// walked backwards with OVERWRITE=false so the LAST entry wins.
std::vector<std::string> gEnvFiles{};

// bunfig `preload = [...]` — modules imported before the entry point.
// ref: bun runtime/jsc_hooks.rs:700-800 (resolve, then import, each in order;
// a miss is fatal with `preload not found <json path>`).
std::vector<std::string> gPreloads{};

// `--tsconfig-override <path>` — one process-wide explicit config for runtime
// resolution. The CLI makes it absolute against its parse cwd before the
// runtime starts, so imports never reinterpret it relative to their own file.
std::string gTsconfigOverride{};

// `--user-agent <STR>` — overrides the default fetch User-Agent.
// ref: bun cli/Arguments.rs:1062 → bun_http::OVERRIDDEN_DEFAULT_USER_AGENT.
std::string gUserAgent{};

// ── the Permission Model (node --permission / --allow-*) ────────────────────
// One instance per process, exactly like node's Environment::permission(). It
// starts DISABLED and every gate below is a no-op in that state, so an ordinary
// run cannot be affected: `gPermission.enabled()` is the first thing each check
// tests.
//
// This lives here, in the runtime's own state, and NOT behind a JS object,
// because it is what the C++ fs/spawn/worker boundaries consult. A model that
// only existed in JS could be reached around by anything that called those
// boundaries directly.
mbun::permission::Model gPermission{};

// The parsed flags, kept so the JS layer can reproduce node's
// getOptionValue('--allow-…') answers (the SecurityWarning loop, the
// ERR_MISSING_OPTION check) and so child_process can copy them into a child's
// NODE_OPTIONS.
mbun::permission::Options gPermissionOptions{};

// Set once, before the model is enabled: `mbun --permission x.js` and
// `NODE_OPTIONS=--permission mbun x.js` both land here (see src/main.cpp).
std::vector<std::string> gPermissionTokens{};
bool gPermissionHasEvalString{false};
std::string gPermissionEntry{};
std::vector<std::string> gPermissionPreloads{};

// Bind the model to the live process: node re-resolves a relative reference
// against the CURRENT cwd on every call (env->GetCwd()), so process.chdir()
// changes what "." means, and a directory grant is decided by a real stat.
void permission_bind_probes() {
    gPermission.set_cwd_provider([] {
        std::error_code ec{};
        const auto cwd{std::filesystem::current_path(ec)};
        return ec ? std::string{} : cwd.string();
    });
    gPermission.set_is_dir_probe([](const std::string& p) {
        std::error_code ec{};
        // Follows symlinks, as node's uv_fs_stat does in WildcardIfDir.
        return std::filesystem::is_directory(std::filesystem::path{p}, ec) && !ec;
    });
}

// node CreateAccessDeniedError (src/permission/permission.cc): an Error whose
// `code` is ERR_ACCESS_DENIED and which carries `permission` (the scope's Name)
// and `resource` (the reference that was refused). The corpus asserts all three.
JSValueRef permission_denied_error(JSContextRef ctx,
                                   mbun::permission::Scope scope,
                                   std::string_view resource,
                                   std::string_view messageOverride = {}) {
    const std::string message{messageOverride.empty()
                                  ? mbun::permission::access_denied_message(scope)
                                  : std::string{messageOverride}};
    JSValueRef m{make_string(ctx, message)};
    JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
    set_prop(ctx, err, "code", make_string(ctx, "ERR_ACCESS_DENIED"));
    set_prop(ctx, err, "permission", make_string(ctx, mbun::permission::scope_name(scope)));
    set_prop(ctx, err, "resource", make_string(ctx, resource));
    return err;
}

// Set when the MODULE LOADER's fs adapter refused a read. The loader's read hook
// can only answer "no bytes" (std::optional), which the require path would report
// as "Cannot find module" — a misleading error for a file that exists and was
// deliberately refused. The require path consults this to report
// ERR_ACCESS_DENIED instead. Cleared on every successful require.
std::optional<std::string> gPermissionDeniedModuleRead{};

// The fs gate, used by every native fs entry point.
//
// Returns true when the operation must NOT proceed, having already stored the
// ERR_ACCESS_DENIED into `*exc`. Audit mode (`--permission-audit`) reports and
// then allows, which is what node's warning_only_ does.
//
// `path` is the raw argument as the script wrote it; the model resolves it
// against the cwd before matching, so `../` cannot walk out of a granted tree.
bool permission_deny_fs(JSContextRef ctx, JSValueRef* exc, std::string_view path, bool write) {
    if (!gPermission.enabled()) return false;
    const auto scope{write ? mbun::permission::Scope::FileSystemWrite
                           : mbun::permission::Scope::FileSystemRead};
    if (gPermission.is_granted(scope, path)) return false;
    if (gPermission.warning_only()) {
        std::println(std::cerr, "Warning: Access to this API has been restricted. Use {} to manage permissions. resource: {}",
                     mbun::permission::scope_flag(scope), path);
        return false;
    }
    if (exc != nullptr) *exc = permission_denied_error(ctx, scope, path);
    return true;
}

// Defined in process_extended.inc; the spawn gate below needs it to recognise a
// re-exec of this same binary.
std::string exe_path();

// The spawn gate.
//
// mbun implements node's worker_threads as a CHILD PROCESS (one mbun process per
// worker — see builtins/node_worker.cppm for why), where node uses a thread. So
// a spawn that is really a worker must be charged to the WorkerThreads scope, not
// to ChildProcess: `--allow-worker` alone has to be enough, exactly as in node.
//
// The allowance is deliberately narrow, because "worker" must not become a
// general spawn escape:
//   * WorkerThreads must be granted, AND
//   * the executable must be THIS binary (a worker re-enters mbun; it can never
//     exec /bin/sh or any other program), AND
//   * the child's env must carry the worker handshake variable.
// The capability that grants is therefore exactly `new Worker(...)`: another mbun
// process, running JS, and — because the permission flags travel to it in argv
// and NODE_OPTIONS — under the same sandbox. Nothing a script could not already
// do with --allow-worker under node.
bool permission_deny_spawn(JSContextRef ctx, JSValueRef* exc, const std::string& file,
                           const std::vector<std::string>& env) {
    if (!gPermission.enabled()) return false;
    if (gPermission.is_granted(mbun::permission::Scope::ChildProcess)) return false;
    if (gPermission.is_granted(mbun::permission::Scope::WorkerThreads) && file == exe_path()) {
        for (const std::string& e : env) {
            if (e.starts_with("MBUN_WORKER_TID=")) return false;
        }
    }
    if (gPermission.warning_only()) {
        std::println(std::cerr, "Warning: {} resource: {}",
                     mbun::permission::access_denied_message(
                         mbun::permission::Scope::ChildProcess),
                     file);
        return false;
    }
    if (exc != nullptr) {
        *exc = permission_denied_error(ctx, mbun::permission::Scope::ChildProcess, file);
    }
    return true;
}

// The non-fs scopes (child process, worker, wasi, addon, inspector, net). Same
// contract as permission_deny_fs.
bool permission_deny_scope(JSContextRef ctx, JSValueRef* exc, mbun::permission::Scope scope,
                           std::string_view resource) {
    if (!gPermission.enabled()) return false;
    if (gPermission.is_granted(scope, resource)) return false;
    if (gPermission.warning_only()) {
        std::println(std::cerr, "Warning: {} resource: {}",
                     mbun::permission::access_denied_message(scope), resource);
        return false;
    }
    if (exc != nullptr) *exc = permission_denied_error(ctx, scope, resource);
    return true;
}

// ── JSC deferred work (post-GC / cross-thread) ──────────────────────────────
// PORT-SOURCE: compat/bun/src/jsc/bindings/JSCTaskScheduler.cpp
//
// JSC does not run a FinalizationRegistry's cleanup callback inside GC. The
// End phase only MOVES the dead holdings onto the registry, then parks the
// callback on VM::deferredWorkTimer (JSFinalizationRegistry::
// finalizeUnconditionally -> scheduleWorkSoon). DeferredWorkTimer is a
// JSRunLoopTimer, so it only fires when someone runs WTF's RunLoop on the JS
// thread. mbun runs its OWN pump and never enters that RunLoop, so the parked
// task sat in DeferredWorkTimer::m_tasks forever: `Bun.gc(true)` cleared
// WeakRefs (finalizeSynchronousJSExecution bumps the weak version) yet no
// FinalizationRegistry callback ever ran -- measured 0/8 finalizers after 60
// forced collections. bun hits the same wall and solves it by REPLACING the
// timer's dispatch (onScheduleWorkSoon -> its own event loop); the equivalent
// with a pump we already own is to drain the queue from the pump.
//
// Same queue carries Atomics.waitAsync resumptions and async WebAssembly
// compile completions, so this is not finalizer-specific plumbing.
//
// doWork() asserts the caller holds the API lock and calls vm.drainMicrotasks()
// after each task, so it must run at a point where re-entering JS is legal --
// i.e. the same places we already drain microtasks, never inside GC itself.
void mbun_drain_deferred_work(JSC::VM& vm) {
    // The API lock is NOT optional here, and its absence does not fail loudly.
    // A parked task calls back into JS (JSFinalizationRegistry::
    // runFinalizationCleanup invokes the user callback), and JSLock::lock is
    // what installs vm.atomStringTable() as the CURRENT THREAD's AtomString
    // table. Compiling/running that callback allocates, an allocation can
    // trigger a sweep, and the sweep destroys cells (JSC::SymbolTable) holding
    // AtomStrings interned in the VM's table -- which AtomStringImpl::remove
    // then looks for in the thread's default table and cannot find:
    //   RELEASE_ASSERT ... WTFCrashWithInfoImpl, on a string as ordinary as
    //   "eventName".
    // The native pump (Runtime::drain_microtasks_) runs OUTSIDE any JS call,
    // so it holds no lock of its own; JSLock is recursive, so taking it here is
    // also correct for the caller that already holds one
    // (__mbunDrainMicrotasksNative, reached from JS).
    JSC::DeferredWorkTimer* deferred{vm.deferredWorkTimer.ptr()};
    if (deferred == nullptr) return;
    // doWork() ends with an unconditional vm.drainMicrotasks(), so it is only
    // safe where a microtask checkpoint is ALREADY expected. That is why the
    // callers are exactly the two checkpoints -- the native pump and
    // __mbunDrainMicrotasksNative -- and why Bun.gc(true) deliberately is not
    // one (see the note in bindings_install.inc's __mbunGcNative). Skipping
    // when nothing is parked keeps the hot tick off the timer's lock; a
    // scheduled task always has a pending ticket, so this cannot hide work.
    if (!deferred->hasAnyPendingWork()) return;
    JSC::JSLockHolder locker{vm};
    // Virtual dispatch through the JSRunLoopTimer base: doWork is the timer's
    // pure-virtual entry, so this needs no exported symbol.
    static_cast<JSC::JSRunLoopTimer*>(deferred)->doWork(vm);
}
