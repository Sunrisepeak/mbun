// CAP-WORKER: real cross-thread Worker execution (node:worker_threads Worker +
// the DOM Worker global). Each Worker owns an INDEPENDENT JSC VM on its own OS
// thread (JSGlobalContextCreate → a fresh JSC::VM/heap, proven to run
// concurrently with the main VM on bun's WebKit fork). Messages are marshalled
// between the two VMs by value: the sender serialises the argument with the JSC
// C API's JSON codec (JSValueCreateJSONString) into a wire string, the bytes
// cross the thread boundary through a mutex-guarded queue, and the receiver
// re-materialises the value with JSON.parse in its own VM. This is a real
// second VM on a real thread — NOT a same-thread shim.
//
// Blueprint: bun src/jsc/web_worker.rs (a WebWorker owns its VirtualMachine on a
// thread, exchanging structured-cloned messages over its own event loop) and
// src/js/node/worker_threads.ts. The wire codec here is the JSON subset of
// structured clone (primitives / arrays / plain objects): the JSC C API exposes
// no SerializedScriptValue entry point, so transferables (ArrayBuffer/MessagePort
// handoff), Date/Map/Set and cyclic graphs are out of this slice's range and
// their tests stay honestly red. The worker VM carries a minimal global surface
// (console + postMessage/onmessage/parentPort + workerData); it has no module
// loader, so only global-`postMessage` workers (incl. `eval: true`) execute —
// module-importing worker fixtures remain deferred.
//
// Included inside runtime.cppm's anonymous namespace (uses common.inc helpers:
// set_fn / make_string / val_to_string / read_file). Registered into the main
// context by engine.inc install_bindings_ as globalThis.__mbunWorkerNative, and
// drained by the main event-loop pump via globalThis.__mbunWorkerDrain.

// ── worker-side minimal runtime bootstrap (evaluated in the worker VM) ───────
inline constexpr std::string_view kWorkerBootstrapJS = R"JS(
(function () {
  const G = globalThis;
  G.self = G;
  let workerData = null;
  try { workerData = G.__mbunWorkerDataJson ? JSON.parse(G.__mbunWorkerDataJson) : null; } catch (e) {}
  const messageListeners = [];
  let onmessageFn = null;
  const dispatch = (data) => {
    const ev = { data: data, type: "message", ports: [], target: G, currentTarget: G };
    if (typeof onmessageFn === "function") { try { onmessageFn(ev); } catch (e) { G.__mbunWorkerReportError(String((e && e.stack) || e)); } }
    for (const cb of messageListeners.slice()) { try { cb(ev); } catch (e) { G.__mbunWorkerReportError(String((e && e.stack) || e)); } }
  };
  const parentPort = {
    postMessage: (v) => { G.postMessage(v); },
    on(type, cb) { if (type === "message") messageListeners.push(cb); return parentPort; },
    once(type, cb) { if (type === "message") { const w = (e) => { const i = messageListeners.indexOf(w); if (i >= 0) messageListeners.splice(i, 1); cb(e); }; messageListeners.push(w); } return parentPort; },
    off(type, cb) { if (type === "message") { const i = messageListeners.indexOf(cb); if (i >= 0) messageListeners.splice(i, 1); } return parentPort; },
    addListener(type, cb) { return parentPort.on(type, cb); },
    removeListener(type, cb) { return parentPort.off(type, cb); },
    addEventListener(type, cb) { if (type === "message") messageListeners.push(cb); },
    removeEventListener(type, cb) { parentPort.off(type, cb); },
    start() {}, close() {}, ref() { return parentPort; }, unref() { return parentPort; },
    get onmessage() { return onmessageFn; }, set onmessage(f) { onmessageFn = f; },
  };
  G.__mbunParentPort = parentPort;
  G.__mbunWorkerData = workerData;
  Object.defineProperty(G, "onmessage", { get() { return onmessageFn; }, set(f) { onmessageFn = f; }, configurable: true });
  G.addEventListener = (type, cb) => { if (type === "message") messageListeners.push(cb); };
  G.removeEventListener = (type, cb) => { const i = messageListeners.indexOf(cb); if (i >= 0) messageListeners.splice(i, 1); };
  // Native delivery hook: C++ hands us one wire string per inbound message.
  G.__mbunWorkerDeliver = (wire) => { let data; try { data = wire === undefined ? undefined : JSON.parse(wire); } catch (e) { data = undefined; } dispatch(data); };
  // Liveness: a worker with a live message listener keeps its loop alive.
  G.__mbunWorkerListenerCount = () => (onmessageFn ? 1 : 0) + messageListeners.length;
  // Minimal require('worker_threads')/('node:worker_threads') inside the worker.
  const wt = { isMainThread: false, parentPort: parentPort, threadId: G.__mbunWorkerThreadId | 0, workerData: workerData, SHARE_ENV: Symbol("nodejs.worker_threads.SHARE_ENV"), resourceLimits: {} };
  const origRequire = G.require;
  G.require = function (id) { if (id === "worker_threads" || id === "node:worker_threads") return wt; if (typeof origRequire === "function") return origRequire.apply(this, arguments); throw new Error("Cannot require '" + id + "' in this worker (no module loader)"); };
})();
)JS";

// ── cross-VM message plumbing ────────────────────────────────────────────────
struct WebWorker {
    int id{0};
    std::thread th;
    std::mutex m;                       // guards inbox/outbox
    std::deque<std::string> inbox;      // parent → worker (wire JSON, "" == undefined)
    std::deque<std::string> outbox;     // worker → parent (complete JSON event objects)
    std::atomic<bool> terminate{false};
    std::atomic<bool> exited{false};
};

std::mutex gWorkerMtx;
std::map<int, std::shared_ptr<WebWorker>> gWorkers;
int gNextWorkerId{1};

// The worker thread currently executing on THIS thread (worker host callbacks
// run on their own thread and recover their WebWorker here).
thread_local WebWorker* gTlsWorker{nullptr};

std::string json_string_quote_(const std::string& s) {
    static constexpr char kHex[]{"0123456789abcdef"};
    std::string out{"\""};
    for (char ch : s) {
        const unsigned char c{static_cast<unsigned char>(ch)};
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out += kHex[(c >> 4) & 0xF]; out += kHex[c & 0xF]; }
            else out += ch;
        }
    }
    out += '"';
    return out;
}

// JSValue → wire string via the C API JSON codec. Non-serialisable values
// (undefined / functions / cyclic) yield "null" (JSON's own drop behaviour).
std::string worker_serialize_(JSContextRef ctx, JSValueRef v) {
    if (v == nullptr) return "null";
    JSValueRef exc{nullptr};
    JSStringRef js{JSValueCreateJSONString(ctx, v, 0, &exc)};
    if (js == nullptr || exc != nullptr) return "null";
    std::string out{to_utf8(js)};
    JSStringRelease(js);
    return out.empty() ? "null" : out;
}

void worker_push_event_(WebWorker* w, const std::string& completeJsonObject) {
    std::lock_guard<std::mutex> lk{w->m};
    w->outbox.push_back(completeJsonObject);
}

// Call a global JS function of one string arg in the worker VM.
void worker_call_global1_(JSContextRef ctx, const char* fnName, const std::string& arg) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSStringRef n{JSStringCreateWithUTF8CString(fnName)};
    JSValueRef fnv{JSObjectGetProperty(ctx, global, n, nullptr)};
    JSStringRelease(n);
    if (fnv == nullptr || !JSValueIsObject(ctx, fnv)) return;
    JSObjectRef fn{JSValueToObject(ctx, fnv, nullptr)};
    if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) return;
    JSValueRef a{make_string(ctx, arg)};
    JSValueRef exc{nullptr};
    JSObjectCallAsFunction(ctx, fn, nullptr, 1, &a, &exc);
}

double worker_listener_count_(JSContextRef ctx) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSStringRef n{JSStringCreateWithUTF8CString("__mbunWorkerListenerCount")};
    JSValueRef fnv{JSObjectGetProperty(ctx, global, n, nullptr)};
    JSStringRelease(n);
    if (fnv == nullptr || !JSValueIsObject(ctx, fnv)) return 0;
    JSObjectRef fn{JSValueToObject(ctx, fnv, nullptr)};
    if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) return 0;
    JSValueRef exc{nullptr};
    JSValueRef r{JSObjectCallAsFunction(ctx, fn, nullptr, 0, nullptr, &exc)};
    if (r == nullptr || exc != nullptr) return 0;
    return JSValueToNumber(ctx, r, nullptr);
}

// ── worker-side host callbacks (run on the worker thread's VM) ───────────────
JSValueRef worker_postMessage_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef argv[], JSValueRef*) {
    WebWorker* w{gTlsWorker};
    if (w != nullptr) {
        std::string wire{argc > 0 ? worker_serialize_(ctx, argv[0]) : "null"};
        worker_push_event_(w, std::string{"{\"id\":"} + std::to_string(w->id) +
                                  ",\"t\":\"message\",\"d\":" + wire + "}");
    }
    return JSValueMakeUndefined(ctx);
}

JSValueRef worker_console_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef argv[], JSValueRef*) {
    std::string line;
    for (std::size_t i{0}; i < argc; ++i) {
        if (i) line += ' ';
        line += val_to_string(ctx, argv[i]);
    }
    line += '\n';
    std::cout << line;
    std::cout.flush();
    return JSValueMakeUndefined(ctx);
}

JSValueRef worker_reportError_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef argv[], JSValueRef*) {
    WebWorker* w{gTlsWorker};
    if (w != nullptr) {
        std::string msg{argc > 0 ? val_to_string(ctx, argv[0]) : std::string{"Error"}};
        worker_push_event_(w, std::string{"{\"id\":"} + std::to_string(w->id) +
                                  ",\"t\":\"error\",\"d\":" + json_string_quote_(msg) + "}");
    }
    return JSValueMakeUndefined(ctx);
}

// ── the worker thread body ───────────────────────────────────────────────────
void worker_thread_main(std::shared_ptr<WebWorker> w, std::string source, std::string workerDataJson,
                        int threadId) {
    gTlsWorker = w.get();
    JSGlobalContextRef ctx{JSGlobalContextCreate(nullptr)};

    // Install the minimal worker global surface.
    {
        JSC::JSLockHolder locker{toJS(ctx)};
        JSObjectRef g{JSContextGetGlobalObject(ctx)};
        set_prop(ctx, g, "__mbunWorkerDataJson",
                 workerDataJson.empty() ? JSValueMakeNull(ctx) : make_string(ctx, workerDataJson));
        set_prop(ctx, g, "__mbunWorkerThreadId", JSValueMakeNumber(ctx, threadId));
        set_fn(ctx, g, "postMessage", worker_postMessage_host);
        set_fn(ctx, g, "__mbunWorkerReportError", worker_reportError_host);
        // console
        JSObjectRef console{JSObjectMake(ctx, nullptr, nullptr)};
        set_fn(ctx, console, "log", worker_console_host);
        set_fn(ctx, console, "info", worker_console_host);
        set_fn(ctx, console, "warn", worker_console_host);
        set_fn(ctx, console, "error", worker_console_host);
        set_fn(ctx, console, "debug", worker_console_host);
        set_prop(ctx, g, "console", console);
    }

    auto eval = [&](std::string_view src) -> bool {
        std::string s{src};
        JSStringRef script{JSStringCreateWithUTF8CString(s.c_str())};
        JSValueRef exc{nullptr};
        JSEvaluateScript(ctx, script, nullptr, nullptr, 1, &exc);
        JSStringRelease(script);
        if (exc != nullptr) {
            std::string msg{val_to_string(ctx, exc)};
            worker_push_event_(w.get(), std::string{"{\"id\":"} + std::to_string(w->id) +
                                            ",\"t\":\"error\",\"d\":" + json_string_quote_(msg) + "}");
            return false;
        }
        return true;
    };

    eval(kWorkerBootstrapJS);
    // W3C Performance / PerformanceObserver are part of the WorkerGlobalScope in
    // bun (and node exposes perf_hooks on every thread). The perf partition is a
    // self-contained IIFE with no native dependency beyond an optional
    // Bun.nanoseconds, so the worker VM gets the same surface as the main one.
    eval(mbun::jsc::builtins::detail::kNodePerfJS);
    eval(source);  // user entry script (error already reported on throw)

    // Event loop: deliver inbound messages, drain microtasks, stay alive while a
    // message listener is registered (else the loop is empty → the worker exits).
    while (!w->terminate.load(std::memory_order_relaxed)) {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> lk{w->m};
            batch.swap(w->inbox);
        }
        for (auto& wire : batch) worker_call_global1_(ctx, "__mbunWorkerDeliver", wire);
        if (JSC::JSGlobalObject* gg{toJS(ctx)}) {
            JSC::JSLockHolder locker{gg->vm()};
            gg->vm().drainMicrotasks();
        }
        bool inboxEmpty;
        { std::lock_guard<std::mutex> lk{w->m}; inboxEmpty = w->inbox.empty(); }
        if (inboxEmpty && worker_listener_count_(ctx) == 0) break;  // empty loop → exit
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    // Emit exit BEFORE flipping `exited` so the parent drain observes the event
    // before it reclaims the worker (see worker_drain_host).
    worker_push_event_(w.get(),
                       std::string{"{\"id\":"} + std::to_string(w->id) + ",\"t\":\"exit\",\"d\":0}");
    w->exited.store(true, std::memory_order_release);
}

// ── main-VM host callbacks (globalThis.__mbunWorkerNative.*) ─────────────────
// spawn(source, workerDataJson) → threadId
JSValueRef worker_spawn_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef argv[], JSValueRef* exc) {
    // Permission Model: node gates the WorkerThreads scope in
    // src/node_worker.cc's Worker::New, before the thread is created — the same
    // place, here. A JS-side check in node:worker_threads would be reachable
    // around via this host function.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::WorkerThreads, ""))
        return JSValueMakeUndefined(ctx);
    std::string source{arg_string(ctx, argc, argv, 0)};
    std::string workerDataJson{arg_string(ctx, argc, argv, 1)};
    int id;
    auto w{std::make_shared<WebWorker>()};
    {
        std::lock_guard<std::mutex> lk{gWorkerMtx};
        id = gNextWorkerId++;
        w->id = id;
        gWorkers.emplace(id, w);
    }
    w->th = std::thread(worker_thread_main, w, std::move(source), std::move(workerDataJson), id);
    w->th.detach();
    return JSValueMakeNumber(ctx, id);
}

// post(id, value) → undefined  (serialised on the parent VM, delivered to inbox)
JSValueRef worker_post_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef argv[], JSValueRef*) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    int id{static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr))};
    std::string wire{argc > 1 ? worker_serialize_(ctx, argv[1]) : "null"};
    std::shared_ptr<WebWorker> w;
    { std::lock_guard<std::mutex> lk{gWorkerMtx}; auto it{gWorkers.find(id)}; if (it != gWorkers.end()) w = it->second; }
    if (w) { std::lock_guard<std::mutex> lk{w->m}; w->inbox.push_back(wire); }
    return JSValueMakeUndefined(ctx);
}

// terminate(id) → undefined
JSValueRef worker_terminate_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef argv[], JSValueRef*) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    int id{static_cast<int>(JSValueToNumber(ctx, argv[0], nullptr))};
    std::shared_ptr<WebWorker> w;
    { std::lock_guard<std::mutex> lk{gWorkerMtx}; auto it{gWorkers.find(id)}; if (it != gWorkers.end()) w = it->second; }
    if (w) w->terminate.store(true, std::memory_order_relaxed);
    return JSValueMakeUndefined(ctx);
}

// drain() → JSON array string of pending events across all workers; reclaims a
// worker once its terminal exit event has been drained.
JSValueRef worker_drain_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                             const JSValueRef[], JSValueRef*) {
    std::string out{"["};
    bool first{true};
    std::vector<int> reclaim;
    {
        std::lock_guard<std::mutex> lk{gWorkerMtx};
        for (auto& [id, w] : gWorkers) {
            std::deque<std::string> batch;
            { std::lock_guard<std::mutex> wl{w->m}; batch.swap(w->outbox); }
            for (auto& ev : batch) { if (!first) out += ','; out += ev; first = false; }
            bool drained;
            { std::lock_guard<std::mutex> wl{w->m}; drained = w->outbox.empty(); }
            if (w->exited.load(std::memory_order_acquire) && drained) reclaim.push_back(id);
        }
        for (int id : reclaim) gWorkers.erase(id);
    }
    out += ']';
    return make_string(ctx, out);
}

// activeCount() → number of workers not yet fully exited (parent-pump liveness)
JSValueRef worker_active_host(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                              const JSValueRef[], JSValueRef*) {
    std::size_t n{0};
    { std::lock_guard<std::mutex> lk{gWorkerMtx}; for (auto& [id, w] : gWorkers) if (!w->exited.load(std::memory_order_acquire)) ++n; }
    return JSValueMakeNumber(ctx, static_cast<double>(n));
}
