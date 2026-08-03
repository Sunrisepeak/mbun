// CAP-NAPI async surface: async contexts, callback scopes, napi_make_callback,
// napi_async_work (real worker threads), threadsafe functions, and the two
// libuv entry points addons in this family link against.
//
// Included by prelude.hpp after napi_binary.inc, in the GLOBAL MODULE FRAGMENT
// (same external-linkage requirement as the rest of the N-API surface: a
// dlopen'd .node resolves these from the executable, and a MISSING one is not a
// soft failure -- the addon fails to load on an undefined symbol before a
// single test case runs, which is what kept this whole family red).
//
// Blueprint: node's src/node_api.cc (async_context / callback scope /
// make_callback / async work / threadsafe function) and bun's
// compat/bun/src/jsc/bindings/napi.cpp. Deviations are noted inline; the big
// one is the loop re-entry vehicle, see "event-loop re-entry" below.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mbun::napi_rt {

// ── event-loop re-entry ─────────────────────────────────────────────────────
// Node hands async-work completions and threadsafe-function calls back to the
// JS thread through libuv (uv_async_send onto the loop). mbun has no libuv and
// its pump is driven from JS globals, so the equivalent vehicle is a 0 ms timer:
//   * a ref'd timer is already counted by __mbun_timers_refd, which is exactly
//     the pump's "does the loop still have work?" test, so pending async work
//     holds the process open without touching pump_event_loop at all;
//   * __mbun_pump_idle_ms sees a due timer and does not park, so a completion
//     lands on the next pump iteration rather than after an idle sleep;
//   * a timer callback is a callback boundary mbun already drains nextTick and
//     microtasks around, which is where node runs these callbacks too.
// The alternative -- a native counter spliced into pump_event_loop's liveness
// expression -- was rejected because it changes the pump for every runtime, not
// just the ones that loaded an addon.
//
// Arming only ever happens on the JS thread. A worker thread that finishes work
// does NOT arm; it cannot touch JSC. Instead the drain timer re-arms itself for
// as long as anything is outstanding, so the poll is already in flight before
// any worker can finish (queue_async_work arms before the thread is spawned).

struct ThreadSafeFunction;

struct AsyncWorkRec {
    napi_env env{nullptr};
    napi_async_execute_callback execute{nullptr};
    napi_async_complete_callback complete{nullptr};
    void* data{nullptr};
    JSC::Strong<JSC::Unknown> resource;
    // 0 = created, 1 = queued, 2 = executing, 3 = executed, 4 = cancelled
    std::atomic<int> state{0};
};

struct AsyncRuntime {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<AsyncWorkRec*> queued;     // accepted, not yet picked up by a worker
    std::deque<AsyncWorkRec*> finished;   // executed or cancelled, awaiting the JS thread
    int running{0};                       // workers inside an execute callback
    int idleWorkers{0};
    int liveWorkers{0};
    bool shuttingDown{false};

    // JS-thread-only fields (no lock).
    bool drainArmed{false};
    int idleDrains{0};
    JSC::Strong<JSC::Unknown> drainFn;
    std::vector<ThreadSafeFunction*> tsfns;

    // node's default UV_THREADPOOL_SIZE. test_async's TestCancel depends on the
    // pool being BOUNDED: it saturates it with sleeping work so the item it
    // then cancels is still queued (napi_cancel_async_work only succeeds
    // before execution starts). An unbounded thread-per-work implementation
    // makes that cancel race and fail.
    static constexpr int kMaxWorkers = 4;

    static AsyncRuntime& singleton() {
        static AsyncRuntime* rt = new AsyncRuntime();  // leaked: alive through exit
        return *rt;
    }
};

// An async-work completion, a threadsafe-function call and a tsfn finalizer are
// all callback boundaries reached from the loop rather than from JS, exactly
// like a deferred GC finalizer. They therefore reuse the finalizer drain's two
// halves verbatim: prepareFinalizerCallback() clears whatever the previous
// boundary left visible through the top exception scope (otherwise the
// callback's FIRST N-API call fails with napi_pending_exception), and
// dispatchFinalizerExceptions() routes whatever escaped to node's
// 'uncaughtException' path -- arming the fatal channel when nothing claims it.
// test_threadsafe_function/uncaught_exception.js pins both directions: the tsfn
// callback throws 'callback error' and the tsfn finalizer then throws
// 'finalizer error', and each must reach a separate 'uncaughtException'
// listener in that order.

inline bool asyncWorkOutstanding() {
    auto& rt = AsyncRuntime::singleton();
    std::lock_guard<std::mutex> lock{rt.mutex};
    return !rt.queued.empty() || !rt.finished.empty() || rt.running > 0;
}

inline bool tsfnOutstanding();
inline void dispatchThreadSafeFunctions(JSC::JSGlobalObject* globalObject);

// Run every completion that a worker has parked. Each completion is a fresh
// callback boundary: its own handle scope, its own exception flush.
inline void runAsyncWorkCompletions(JSC::JSGlobalObject* globalObject) {
    auto& rt = AsyncRuntime::singleton();
    for (;;) {
        AsyncWorkRec* work{nullptr};
        {
            std::lock_guard<std::mutex> lock{rt.mutex};
            if (rt.finished.empty()) return;
            work = rt.finished.front();
            rt.finished.pop_front();
        }
        const napi_status status{work->state.load() == 4 ? napi_cancelled : napi_ok};
        // The work item is NOT freed here: node's contract is that the addon
        // owns the handle until napi_delete_async_work, and every addon in this
        // family calls that from INSIDE the completion callback below
        // (test_async's Complete and RepeatedWorkComplete both do). Freeing here
        // would double-free through that call -- and, symmetrically, nothing
        // after the callback may dereference `work` again: reading work->env to
        // report an escaped exception was a use-after-free that segfaulted
        // test-loop.js on its first completion. Everything the tail needs is
        // read out first.
        napi_env env{work->env};
        napi_async_complete_callback complete{work->complete};
        void* data{work->data};
        if (complete != nullptr) {
            ScopedHandleScope scope;
            prepareFinalizerCallback(env);
            complete(env, status, data);
            (void)dispatchFinalizerExceptions(env);
        }
    }
}

inline void armAsyncDrain(JSC::JSGlobalObject* globalObject, int delayMs);

// The timer callback. Runs completions, runs threadsafe dispatch, and re-arms
// while anything is still outstanding.
inline JSC::JSValue asyncDrainTick(JSC::JSGlobalObject* globalObject) {
    auto& rt = AsyncRuntime::singleton();
    rt.drainArmed = false;
    runAsyncWorkCompletions(globalObject);
    dispatchThreadSafeFunctions(globalObject);
    if (asyncWorkOutstanding() || tsfnOutstanding()) {
        // A poll that finds nothing ramps 0 -> 1 ms so a long-running execute
        // callback (test_async's uv_sleep(1000)) does not spin a core; the
        // first few polls stay at 0 ms so the common case -- work that finishes
        // immediately -- still completes on the very next pump iteration.
        const int delay{rt.idleDrains++ < 2 ? 0 : 1};
        armAsyncDrain(globalObject, delay);
    } else {
        rt.idleDrains = 0;
    }
    return JSC::jsUndefined();
}

inline void armAsyncDrain(JSC::JSGlobalObject* globalObject, int delayMs) {
    auto& rt = AsyncRuntime::singleton();
    if (rt.drainArmed || globalObject == nullptr) return;
    JSC::VM& vm{JSC::getVM(globalObject)};
    if (!rt.drainFn) {
        JSC::JSNativeStdFunction* fn{JSC::JSNativeStdFunction::create(
            vm, globalObject, 0, WTF::String::fromLatin1("__mbunNapiAsyncDrain"),
            [](JSC::JSGlobalObject* g, JSC::CallFrame*) -> JSC::EncodedJSValue {
                return JSC::JSValue::encode(asyncDrainTick(g));
            })};
        rt.drainFn.set(vm, fn);
    }
    JSContextRef ctx{toRef(globalObject)};
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSStringRef name{JSStringCreateWithUTF8CString("setTimeout")};
    JSValueRef setTimeoutValue{JSObjectGetProperty(ctx, global, name, nullptr)};
    JSStringRelease(name);
    if (setTimeoutValue == nullptr || !JSValueIsObject(ctx, setTimeoutValue)) return;
    JSObjectRef setTimeoutFn{JSValueToObject(ctx, setTimeoutValue, nullptr)};
    if (setTimeoutFn == nullptr || !JSObjectIsFunction(ctx, setTimeoutFn)) return;
    const JSValueRef args[2]{toRef(globalObject, rt.drainFn.get()),
                             JSValueMakeNumber(ctx, delayMs)};
    JSValueRef exc{nullptr};
    (void)JSObjectCallAsFunction(ctx, setTimeoutFn, nullptr, 2, args, &exc);
    if (exc == nullptr) rt.drainArmed = true;
}

// ── worker pool ─────────────────────────────────────────────────────────────

inline void asyncWorkerLoop() {
    auto& rt = AsyncRuntime::singleton();
    for (;;) {
        AsyncWorkRec* work{nullptr};
        {
            std::unique_lock<std::mutex> lock{rt.mutex};
            while (rt.queued.empty()) {
                // Idle out rather than parking a thread forever: an addon that
                // queues one work item at startup should not keep four threads
                // alive for the life of the process.
                ++rt.idleWorkers;
                const bool got{rt.cv.wait_for(lock, std::chrono::seconds(2),
                                              [&rt] { return !rt.queued.empty(); })};
                --rt.idleWorkers;
                if (!got) {
                    --rt.liveWorkers;
                    return;
                }
            }
            work = rt.queued.front();
            rt.queued.pop_front();
            int expected{1};
            if (!work->state.compare_exchange_strong(expected, 2)) {
                continue;  // cancelled between queue and pickup
            }
            ++rt.running;
        }
        if (work->execute != nullptr) work->execute(work->env, work->data);
        {
            std::lock_guard<std::mutex> lock{rt.mutex};
            --rt.running;
            work->state.store(3);
            rt.finished.push_back(work);
        }
    }
}

inline void ensureAsyncWorker() {
    auto& rt = AsyncRuntime::singleton();
    std::lock_guard<std::mutex> lock{rt.mutex};
    if (rt.idleWorkers > 0 || rt.liveWorkers >= AsyncRuntime::kMaxWorkers) {
        rt.cv.notify_one();
        return;
    }
    ++rt.liveWorkers;
    try {
        std::thread{asyncWorkerLoop}.detach();
    } catch (...) {
        --rt.liveWorkers;
    }
}

// ── threadsafe functions ────────────────────────────────────────────────────

struct ThreadSafeFunction {
    napi_env env{nullptr};
    JSC::Strong<JSC::Unknown> jsFunc;    // may be empty when call_js_cb is supplied
    JSC::Strong<JSC::Unknown> resource;
    napi_threadsafe_function_call_js callJs{nullptr};
    void* context{nullptr};
    napi_finalize finalizeCb{nullptr};
    void* finalizeData{nullptr};
    void* finalizeHint{nullptr};
    std::size_t maxQueueSize{0};  // 0 = unbounded

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<void*> queue;
    int threadCount{0};
    bool closing{false};   // last thread released; drain then finalize
    bool aborted{false};
    bool finalized{false};
    bool refd{true};
};

inline bool tsfnOutstanding() {
    auto& rt = AsyncRuntime::singleton();
    for (ThreadSafeFunction* tsfn : rt.tsfns) {
        std::lock_guard<std::mutex> lock{tsfn->mutex};
        if (tsfn->finalized) continue;
        if (!tsfn->queue.empty()) return true;
        if (tsfn->closing) return true;
        if (tsfn->refd && tsfn->threadCount > 0) return true;
    }
    return false;
}

// Default call_js_cb (node: the js_callback is invoked with no arguments and
// `undefined` receiver when the addon supplied none).
inline void tsfnDefaultCall(ThreadSafeFunction* tsfn, JSC::JSGlobalObject* globalObject) {
    JSC::JSValue fn{tsfn->jsFunc ? tsfn->jsFunc.get() : JSC::JSValue()};
    if (fn.isEmpty() || !fn.isCallable()) return;
    auto callData = JSC::getCallData(fn);
    JSC::MarkedArgumentBuffer args;
    (void)JSC::call(globalObject, fn, callData, globalObject->globalThis(), args);
}

inline void dispatchThreadSafeFunctions(JSC::JSGlobalObject* globalObject) {
    auto& rt = AsyncRuntime::singleton();
    // Snapshot: a finalizer can create or destroy tsfns while we iterate.
    std::vector<ThreadSafeFunction*> snapshot{rt.tsfns};
    for (ThreadSafeFunction* tsfn : snapshot) {
        for (;;) {
            void* data{nullptr};
            bool have{false};
            {
                std::lock_guard<std::mutex> lock{tsfn->mutex};
                if (tsfn->finalized) break;
                if (!tsfn->queue.empty() && !tsfn->aborted) {
                    data = tsfn->queue.front();
                    tsfn->queue.pop_front();
                    have = true;
                }
            }
            if (!have) break;
            tsfn->cv.notify_all();  // a blocking producer may be waiting on space
            ScopedHandleScope scope;
            prepareFinalizerCallback(tsfn->env);
            if (tsfn->callJs != nullptr) {
                tsfn->callJs(tsfn->env,
                             tsfn->jsFunc ? toNapiValue(tsfn->jsFunc.get()) : nullptr,
                             tsfn->context, data);
            } else {
                tsfnDefaultCall(tsfn, globalObject);
            }
            (void)dispatchFinalizerExceptions(tsfn->env);
        }
        bool runFinalize{false};
        {
            std::lock_guard<std::mutex> lock{tsfn->mutex};
            if (!tsfn->finalized && tsfn->closing && (tsfn->queue.empty() || tsfn->aborted)) {
                tsfn->finalized = true;
                runFinalize = true;
            }
        }
        if (!runFinalize) continue;
        if (tsfn->finalizeCb != nullptr) {
            ScopedHandleScope scope;
            prepareFinalizerCallback(tsfn->env);
            tsfn->finalizeCb(tsfn->env, tsfn->finalizeData, tsfn->finalizeHint);
            (void)dispatchFinalizerExceptions(tsfn->env);
        }
        std::erase(rt.tsfns, tsfn);
        delete tsfn;
    }
}

}  // namespace mbun::napi_rt

// ── async contexts (port: node_api.cc napi_async_init/napi_async_destroy) ───
// node builds an async_context_frame with an async_id pair for async_hooks.
// mbun has no native async_hooks provider, so the context is the resource +
// name pair the addon handed us, kept alive for the lifetime of the context.
// The three tests that touch it (make_callback, callback_scope, tsfn) only
// require that the handle round-trips and that the resource stays reachable.
struct napi_async_context__ {
    napi_env env{nullptr};
    JSC::Strong<JSC::Unknown> resource;
    JSC::Strong<JSC::Unknown> name;
};

struct napi_callback_scope__ {
    napi_env env{nullptr};
    napi_async_context context{nullptr};
    JSC::Strong<JSC::Unknown> resource;
};

extern "C" napi_status napi_async_init(napi_env env, napi_value async_resource,
                                       napi_value async_resource_name,
                                       napi_async_context* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, async_resource_name);
    NAPI_CHECK_ARG(env, result);
    auto* context = new napi_async_context__();
    context->env = env;
    JSC::VM& vm{env->vm()};
    if (async_resource != nullptr) {
        JSC::JSValue resource{mbun::napi_rt::toJSValue(async_resource)};
        if (!resource.isEmpty()) context->resource.set(vm, resource);
    }
    JSC::JSValue name{mbun::napi_rt::toJSValue(async_resource_name)};
    if (!name.isEmpty()) context->name.set(vm, name);
    *result = context;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_async_destroy(napi_env env, napi_async_context async_context) {
    // No throw scope: node's napi_async_destroy is documented to work with a
    // pending exception (test_callback_scope calls it right after rethrowing
    // one), and NAPI_PREAMBLE would report napi_pending_exception instead.
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, async_context);
    delete async_context;
    return mbun::napi_rt::clearLastError(env);
}

// ── callback scopes (port: node_api.cc napi_open/close_callback_scope) ──────

extern "C" napi_status napi_open_callback_scope(napi_env env, napi_value resource_object,
                                                napi_async_context context,
                                                napi_callback_scope* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    auto* scope = new napi_callback_scope__();
    scope->env = env;
    scope->context = context;
    if (resource_object != nullptr) {
        JSC::JSValue resource{mbun::napi_rt::toJSValue(resource_object)};
        if (!resource.isEmpty()) scope->resource.set(env->vm(), resource);
    }
    *result = scope;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
    // Same "must survive a pending exception" contract as napi_async_destroy:
    // binding.c's RunInCallbackScope closes the scope AFTER rethrowing whatever
    // the user callback threw, and node lets that succeed.
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, scope);
    delete scope;
    return mbun::napi_rt::clearLastError(env);
}

// ── napi_make_callback (port: node_api.cc) ─────────────────────────────────
// node runs the callback inside an InternalCallbackScope, which only performs a
// microtask/nextTick checkpoint when it is the OUTERMOST scope. Every call site
// mbun can reach is nested inside a JS evaluation (the addon method was invoked
// from JS), so the checkpoint is deliberately omitted: draining JSC's microtask
// queue from inside a host call reorders promise continuations against the
// evaluation that is still on the stack.
extern "C" napi_status napi_make_callback(napi_env env, napi_async_context async_context,
                                          napi_value recv, napi_value func, size_t argc,
                                          const napi_value* argv, napi_value* result) {
    (void)async_context;
    return napi_call_function(env, recv, func, argc, argv, result);
}

// ── async work (port: node_api.cc + node::ThreadPoolWork) ──────────────────

struct napi_async_work__ : mbun::napi_rt::AsyncWorkRec {};

extern "C" napi_status napi_create_async_work(napi_env env, napi_value async_resource,
                                              napi_value async_resource_name,
                                              napi_async_execute_callback execute,
                                              napi_async_complete_callback complete, void* data,
                                              napi_async_work* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, execute);
    NAPI_CHECK_ARG(env, result);
    auto* work = new napi_async_work__();
    work->env = env;
    work->execute = execute;
    work->complete = complete;
    work->data = data;
    if (async_resource != nullptr) {
        JSC::JSValue resource{mbun::napi_rt::toJSValue(async_resource)};
        if (!resource.isEmpty()) work->resource.set(env->vm(), resource);
    }
    (void)async_resource_name;
    *result = work;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_delete_async_work(napi_env env, napi_async_work work) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, work);
    delete work;
    return mbun::napi_rt::clearLastError(env);
}

extern "C" napi_status napi_queue_async_work(napi_env env, napi_async_work work) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, work);
    auto& rt = mbun::napi_rt::AsyncRuntime::singleton();
    {
        std::lock_guard<std::mutex> lock{rt.mutex};
        work->state.store(1);
        rt.queued.push_back(work);
    }
    // Arm the drain BEFORE waking a worker: arming is JS-thread-only, and the
    // poll must already be in flight by the time the worker can finish.
    rt.idleDrains = 0;
    mbun::napi_rt::armAsyncDrain(env->globalObject(), 0);
    mbun::napi_rt::ensureAsyncWorker();
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_cancel_async_work(napi_env env, napi_async_work work) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, work);
    auto& rt = mbun::napi_rt::AsyncRuntime::singleton();
    {
        std::lock_guard<std::mutex> lock{rt.mutex};
        int expected{1};
        if (!work->state.compare_exchange_strong(expected, 4)) {
            // Already executing or done: node returns napi_generic_failure.
            return mbun::napi_rt::setLastError(env, napi_generic_failure);
        }
        std::erase(rt.queued, static_cast<mbun::napi_rt::AsyncWorkRec*>(work));
        rt.finished.push_back(work);
    }
    mbun::napi_rt::armAsyncDrain(env->globalObject(), 0);
    NAPI_RETURN_SUCCESS(env);
}

// ── uv event loop handle ───────────────────────────────────────────────────
// mbun has no libuv loop. Addons in this family only ever pass the handle back
// to uv_queue_work (shimmed below), which ignores it, so a stable non-null
// token satisfies both the ABI and the "must not be NULL" assertions.
extern "C" napi_status napi_get_uv_event_loop(napi_env env, struct uv_loop_s** loop) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, loop);
    static int mbunFakeUvLoop{0};
    *loop = reinterpret_cast<struct uv_loop_s*>(&mbunFakeUvLoop);
    return mbun::napi_rt::clearLastError(env);
}

// ── threadsafe functions (port: node_api.cc ThreadSafeFunction) ────────────

extern "C" napi_status napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource, napi_value async_resource_name,
    size_t max_queue_size, size_t initial_thread_count, void* thread_finalize_data,
    napi_finalize thread_finalize_cb, void* context, napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, async_resource_name);
    NAPI_CHECK_ARG(env, result);
    NAPI_RETURN_EARLY_IF_FALSE(env, initial_thread_count > 0, napi_invalid_arg);
    // node: func may be NULL only when call_js_cb is supplied.
    NAPI_RETURN_EARLY_IF_FALSE(env, func != nullptr || call_js_cb != nullptr, napi_invalid_arg);

    auto* tsfn = new mbun::napi_rt::ThreadSafeFunction();
    tsfn->env = env;
    tsfn->callJs = call_js_cb;
    tsfn->context = context;
    tsfn->finalizeCb = thread_finalize_cb;
    tsfn->finalizeData = thread_finalize_data;
    tsfn->finalizeHint = nullptr;
    tsfn->maxQueueSize = max_queue_size;
    tsfn->threadCount = static_cast<int>(initial_thread_count);
    JSC::VM& vm{env->vm()};
    if (func != nullptr) {
        JSC::JSValue fn{mbun::napi_rt::toJSValue(func)};
        if (!fn.isEmpty()) tsfn->jsFunc.set(vm, fn);
    }
    if (async_resource != nullptr) {
        JSC::JSValue resource{mbun::napi_rt::toJSValue(async_resource)};
        if (!resource.isEmpty()) tsfn->resource.set(vm, resource);
    }
    auto& rt = mbun::napi_rt::AsyncRuntime::singleton();
    rt.tsfns.push_back(tsfn);
    rt.idleDrains = 0;
    mbun::napi_rt::armAsyncDrain(env->globalObject(), 0);
    *result = reinterpret_cast<napi_threadsafe_function>(tsfn);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_threadsafe_function_context(napi_threadsafe_function func,
                                                            void** result) {
    if (func == nullptr || result == nullptr) return napi_invalid_arg;
    *result = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func)->context;
    return napi_ok;
}

extern "C" napi_status napi_call_threadsafe_function(
    napi_threadsafe_function func, void* data, napi_threadsafe_function_call_mode is_blocking) {
    if (func == nullptr) return napi_invalid_arg;
    auto* tsfn = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func);
    std::unique_lock<std::mutex> lock{tsfn->mutex};
    if (tsfn->aborted) return napi_closing;
    if (tsfn->closing) return napi_closing;
    if (tsfn->maxQueueSize > 0 && tsfn->queue.size() >= tsfn->maxQueueSize) {
        if (is_blocking != napi_tsfn_blocking) return napi_queue_full;
        // A blocking call from the JS thread itself would deadlock -- nothing
        // can drain the queue while we hold the loop. node reports that.
        tsfn->cv.wait(lock, [tsfn] {
            return tsfn->queue.size() < tsfn->maxQueueSize || tsfn->aborted || tsfn->closing;
        });
        if (tsfn->aborted || tsfn->closing) return napi_closing;
    }
    tsfn->queue.push_back(data);
    return napi_ok;
}

extern "C" napi_status napi_acquire_threadsafe_function(napi_threadsafe_function func) {
    if (func == nullptr) return napi_invalid_arg;
    auto* tsfn = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func);
    std::lock_guard<std::mutex> lock{tsfn->mutex};
    if (tsfn->closing || tsfn->aborted) return napi_closing;
    ++tsfn->threadCount;
    return napi_ok;
}

extern "C" napi_status napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
    if (func == nullptr) return napi_invalid_arg;
    auto* tsfn = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func);
    {
        std::lock_guard<std::mutex> lock{tsfn->mutex};
        if (tsfn->threadCount <= 0) return napi_invalid_arg;
        --tsfn->threadCount;
        if (mode == napi_tsfn_abort) tsfn->aborted = true;
        // The last thread letting go closes the function: the queue drains (or
        // is dropped, when aborted) and the finalizer runs, both on the JS
        // thread, in the next drain tick.
        if (tsfn->threadCount == 0) tsfn->closing = true;
    }
    tsfn->cv.notify_all();
    return napi_ok;
}

extern "C" napi_status napi_ref_threadsafe_function(napi_env env, napi_threadsafe_function func) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, func);
    auto* tsfn = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func);
    {
        std::lock_guard<std::mutex> lock{tsfn->mutex};
        tsfn->refd = true;
    }
    mbun::napi_rt::armAsyncDrain(env->globalObject(), 0);
    return mbun::napi_rt::clearLastError(env);
}

extern "C" napi_status napi_unref_threadsafe_function(napi_env env, napi_threadsafe_function func) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, func);
    auto* tsfn = reinterpret_cast<mbun::napi_rt::ThreadSafeFunction*>(func);
    std::lock_guard<std::mutex> lock{tsfn->mutex};
    tsfn->refd = false;
    return mbun::napi_rt::clearLastError(env);
}

// ── libuv shims ────────────────────────────────────────────────────────────
// Node-API addons are allowed to call into libuv directly, and several in this
// corpus do (test_async uses uv_sleep, test_callback_scope uses uv_queue_work).
// Those are UNDEFINED SYMBOLS in the .node: without a definition in the host
// the addon fails to dlopen and every case in the file dies, including the ones
// that never reach the uv call. mbun does not link libuv, so the two entry
// points this family needs are provided on top of the async-work pool above.
// uv_work_t is opaque here: the pointer round-trips untouched.
extern "C" void uv_sleep(unsigned int msec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}

namespace mbun::napi_rt {
struct UvWorkShim {
    void* req{nullptr};
    void (*work)(void*){nullptr};
    void (*afterWork)(void*, int){nullptr};
};

inline void uvWorkExecute(napi_env, void* data) {
    auto* shim = static_cast<UvWorkShim*>(data);
    if (shim->work != nullptr) shim->work(shim->req);
}

inline void uvWorkComplete(napi_env env, napi_status status, void* data) {
    auto* shim = static_cast<UvWorkShim*>(data);
    if (shim->afterWork != nullptr) {
        shim->afterWork(shim->req, status == napi_cancelled ? -125 /* UV_ECANCELED */ : 0);
    }
    delete shim;
}
}  // namespace mbun::napi_rt

extern "C" int uv_queue_work(void* loop, void* req, void (*work_cb)(void*),
                             void (*after_work_cb)(void*, int)) {
    (void)loop;
    auto& state = mbun::napi_rt::NapiState::singleton();
    if (state.envs.empty()) return -22 /* UV_EINVAL */;
    napi_env env{state.envs.back().get()};
    auto* shim = new mbun::napi_rt::UvWorkShim{req, work_cb, after_work_cb};
    napi_async_work work{nullptr};
    if (napi_create_async_work(env, nullptr, nullptr, mbun::napi_rt::uvWorkExecute,
                               mbun::napi_rt::uvWorkComplete, shim, &work) != napi_ok) {
        delete shim;
        return -22;
    }
    if (napi_queue_async_work(env, work) != napi_ok) {
        (void)napi_delete_async_work(env, work);
        delete shim;
        return -22;
    }
    return 0;
}
