// ── __mbunWatchNative: inotify backend for node:fs.watch ────────────────────
// Powers the node:fs FSWatcher / fs.watch / fs.watchFile JS layer
// (builtins/node_fs_watch.cppm). mbun.watcher.inotify (FsWatcher) does the real
// work -- inotify_init1/inotify_add_watch, the recursive subtree crawl, and the
// rename-vs-change classification -- following bun's PathWatcher blueprint
// (bun-ref/src/watcher/, path_watcher.rs). This bridge only marshals handles and
// event records across the JSC C API boundary.
//
// Handles are integer ids into a process-global registry, same rationale as
// __mbunSqliteNative / __mbunNetNative: one JS thread owns them, so no locking.
//
// Threading note: bun runs the directory crawl on a work pool and hands events
// back through a mutex-guarded PathWatcherManager -- the arrangement its
// fs.watch.deadlock regression test targets (close() racing the pool scan
// re-entering a non-recursive mutex). mbun crawls synchronously in watch() and
// drains from the single-threaded reactor, so that race has no analogue here.

#if defined(__linux__)

// Live FsWatcher objects keyed by integer handle. FsWatcher is move-only; the
// map owns it and drops (closing the inotify fd) on erase.
std::unordered_map<int, mbun::watcher::FsWatcher>& watch_registry() {
    static std::unordered_map<int, mbun::watcher::FsWatcher> gWatchers;
    return gWatchers;
}
int& watch_next_handle() {
    static int gNext{1};
    return gNext;
}

// Throw a JS Error carrying .code/.errno/.path/.filename/.syscall, matching the
// shape node's fs.watch errors carry (EPERM/ENOENT/ENOSPC from
// inotify_add_watch). node builds these with UVException (src/node_errors.cc):
// the message is "<CODE>: <uv_strerror text>, watch '<path>'" — the LIBUV
// wording, not the C library's strerror text, which is why the description is
// tabulated here instead of forwarded from BackendError::message. Both `path`
// and `filename` are set: internal/fs/watchers.js assigns `error.filename` on
// top of what UVException already put in `error.path`, and a test that reads
// only `filename` (test-fs-watch-enoent) saw undefined while `path` was right.
JSValueRef watch_throw(JSContextRef ctx, JSValueRef* exc, const mbun::watcher::BackendError& err,
                       const std::string& path) {
    if (exc != nullptr) {
        const std::string code{err.code == ENOENT   ? "ENOENT"
                               : err.code == EACCES ? "EACCES"
                               : err.code == EPERM  ? "EPERM"
                               : err.code == ENOSPC ? "ENOSPC"
                               : err.code == ENOTDIR ? "ENOTDIR"
                                                    : "EINVAL"};
        // libuv's uv_strerror text per code. ENOSPC is the one node overrides
        // (watchers.js): an exhausted inotify budget is reported as the watcher
        // limit rather than "no space left on device".
        const std::string desc{err.code == ENOENT   ? "no such file or directory"
                               : err.code == EACCES ? "permission denied"
                               : err.code == EPERM  ? "operation not permitted"
                               : err.code == ENOSPC ? "System limit for number of file watchers reached"
                               : err.code == ENOTDIR ? "not a directory"
                                                    : "invalid argument"};
        JSValueRef m{make_string(ctx, std::format("{}: {}, watch '{}'", code, desc, path))};
        JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
        set_prop(ctx, e, "code", make_string(ctx, code));
        set_prop(ctx, e, "errno", JSValueMakeNumber(ctx, -err.code));
        set_prop(ctx, e, "path", make_string(ctx, path));
        set_prop(ctx, e, "filename", make_string(ctx, path));
        set_prop(ctx, e, "syscall", make_string(ctx, "watch"));
        *exc = e;
    }
    return JSValueMakeUndefined(ctx);
}

// start(path, recursive) → integer handle. Throws on inotify failure.
JSValueRef watchn_start_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    std::string path{arg_string(ctx, argc, args, 0)};
    // THE PERMISSION GATE. inotify_add_watch reads directory contents and file
    // metadata, so node classifies fs.watch as a FileSystemRead operation
    // (src/fs_event_wrap.cc FSEventWrap::Start →
    // THROW_IF_INSUFFICIENT_PERMISSIONS(kFileSystemRead)). Without this check
    // __mbunWatchNative.start was a way to observe any path in the filesystem
    // — including names of files under a directory the model never granted —
    // from inside a --permission sandbox.
    if (permission_deny_fs(ctx, exc, path, false)) return JSValueMakeUndefined(ctx);
    const bool recursive{argc > 1 && JSValueToBoolean(ctx, args[1])};
    mbun::watcher::FsWatcher w{};
    if (auto ok{w.watch(path, recursive)}; !ok) {
        return watch_throw(ctx, exc, ok.error(), path);
    }
    const int handle{watch_next_handle()++};
    watch_registry().emplace(handle, std::move(w));
    return JSValueMakeNumber(ctx, static_cast<double>(handle));
}

// poll(handle) → array of { name, kind, isDir }. Non-blocking: the reactor calls
// this every drain, so it must never wait for an event to show up.
JSValueRef watchn_poll_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    JSObjectRef out{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    auto it{watch_registry().find(handle)};
    if (it == watch_registry().end()) return out;
    const auto events{it->second.poll(0)};
    for (unsigned i{0}; i < events.size(); ++i) {
        const auto& ev{events[i]};
        JSObjectRef rec{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, rec, "name", make_string(ctx, ev.name));
        set_prop(ctx, rec, "kind", make_string(ctx, std::string{mbun::watcher::change_kind_name(ev.kind)}));
        set_prop(ctx, rec, "isDir", JSValueMakeBoolean(ctx, ev.is_dir));
        JSObjectSetPropertyAtIndex(ctx, out, i, rec, nullptr);
    }
    return out;
}

// stop(handle) — idempotent; drops the FsWatcher (closing the inotify fd).
JSValueRef watchn_stop_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int handle{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    watch_registry().erase(handle);
    return JSValueMakeUndefined(ctx);
}

void install_watch_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef wn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, wn, "start", watchn_start_cb);
    set_fn(ctx, wn, "poll", watchn_poll_cb);
    set_fn(ctx, wn, "stop", watchn_stop_cb);
    set_prop(ctx, global, "__mbunWatchNative", wn);
}

#else

// No inotify here; the JS layer throws ERR_FEATURE_UNAVAILABLE_ON_PLATFORM when
// __mbunWatchNative is absent. kqueue/IOCP backends are DEFERRED, mirroring
// mbun.watcher.backend's platform seam.
void install_watch_native(JSContextRef, JSObjectRef) {}

#endif
