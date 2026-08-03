// T3.2 event loop — JSC microtask hook smoke test.
//
// The pure scheduler (mbun.jsc.event_loop) owns native task/timer/microtask
// scheduling; JavaScriptCore owns its own Promise-job (microtask) queue. This
// smoke test proves the JSC side: evaluating script that schedules a Promise
// job drains that microtask so its side effects are observable on the next
// eval — the semantic the native loop hooks into
// (bun: JSC__JSGlobalObject__drainMicrotasks, src/jsc/event_loop.rs).
//
// NOTE: the host global `queueMicrotask` is NOT a JSC builtin — it is injected
// by the runtime (Bun/Node/Web). The bare C-API JSGlobalContext used here only
// has core ECMAScript builtins, so Promise is available but queueMicrotask is
// not. Binding host globals (queueMicrotask, setTimeout, …) onto the global
// object is part of the runtime layer — DEFERRED with the native microtask hook
// below. Promise jobs alone are enough to smoke JSC's microtask draining.
//
// DEFERRED(S1 → deeper JSC internals): binding the *native* event loop to JSC's
// microtask queue in both directions — native queueMicrotask injection and an
// explicit drainMicrotasks pump interleaved with the timer wheel — needs C++
// PrivateHeaders (JSC::JSGlobalObject::drainMicrotasks / setMicrotaskRunLoop),
// which land when modules/jsc sinks C++ internals (design §3.3 milestone 3).
// The C API path used here relies on JSEvaluateScript's implicit end-of-script
// microtask drain, which is sufficient to smoke the hook.
import std;
import mbun.compat.jsc;
import mbun.jsc.event_loop;

namespace {

int gFailed = 0;

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

}  // namespace

int main() {
    using mbun::compat::jsc::eval;

    // Promise.then schedules a microtask; after the eval returns, JSC has
    // drained it, so the global side effect is visible on the next eval.
    {
        auto setup = eval("globalThis.__mt = 0; Promise.resolve().then(() => { globalThis.__mt = 1; }); 0");
        expect(setup.has_value(), "eval scheduling a promise microtask succeeds");
        auto observed = eval("globalThis.__mt");
        expect(observed.has_value() && *observed == 1.0,
               "JSC drained the Promise.then microtask (observed __mt === 1)");
    }

    // Promise-job ordering: two jobs run FIFO, after synchronous code.
    {
        auto r = eval(
            "globalThis.__order = [];"
            "globalThis.__order.push('sync');"
            "Promise.resolve().then(() => globalThis.__order.push('mt1'));"
            "Promise.resolve().then(() => globalThis.__order.push('mt2'));"
            "0");
        expect(r.has_value(), "eval scheduling promise jobs succeeds");
        auto joined = eval("globalThis.__order.join(',') === 'sync,mt1,mt2' ? 1 : 0");
        expect(joined.has_value() && *joined == 1.0,
               "Promise jobs run FIFO after sync code (sync,mt1,mt2)");
    }

    // Chained promise microtasks resolve to the final value once all jobs drain.
    {
        auto r = eval("Promise.resolve(1).then(x => x + 1).then(x => x + 40); 0");
        expect(r.has_value(), "chained promise eval succeeds");
        auto r2 = eval(
            "globalThis.__chain = 0;"
            "Promise.resolve(1).then(x => x + 1).then(x => { globalThis.__chain = x + 40; });"
            "0");
        expect(r2.has_value(), "chained promise with side effect succeeds");
        auto observed = eval("globalThis.__chain");
        expect(observed.has_value() && *observed == 42.0,
               "chained Promise.then microtasks fully drained (__chain === 42)");
    }

    // Sanity: the native scheduler drains its own microtask queue independently
    // of JSC (the two queues are separate; the native side is unit-tested in
    // test_event_loop.cpp — here we just confirm the module links + runs).
    {
        mbun::jsc::event_loop::EventLoop loop;
        int ran = 0;
        loop.enqueue_microtask([&] { ++ran; });
        loop.drain_microtasks();
        expect(ran == 1, "native EventLoop drains its own microtask queue");
    }

    if (gFailed > 0) {
        std::println("test_event_loop_jsc_smoke: {} failed", gFailed);
        return 1;
    }
    std::println("test_event_loop_jsc_smoke: ok");
    return 0;
}
