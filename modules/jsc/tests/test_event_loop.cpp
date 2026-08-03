// T3.2 event loop — pure-logic scheduler vectors (S0 stage, JSC-free).
//
// Exercises the deterministic scheduling core of mbun.jsc.event_loop with
// injectable callbacks + injectable virtual clock: task FIFO order,
// microtask-after-task draining, process.nextTick priority, the setTimeout /
// setInterval / clearTimeout min-heap wheel (deadline + FIFO tiebreak, clear,
// interval reschedule, in-callback clear), and run_until_idle orchestration.
//
// Vectors are derived from bun's event-loop/timer semantics (src/jsc/
// event_loop.rs, src/runtime/timer/Timer.zig, src/event_loop/EventLoopTimer.zig)
// and, where a bun test is pure ordering, ported faithfully:
//   - microtask nesting order: test/js/web/timers/microtask.test.js
//
// SKIPPED / DEFERRED(S1) — runtime timer behaviour that needs the bun:test
// runner (T3.4) + real/fake wall-clock timers, not a pure scheduler:
//   test/js/web/timers/{setTimeout,setInterval,setImmediate,setImmediate2}.test,
//   *-unref-fixture*, *-leak-fixture*, clearImmediate-gc, performance*,
//   process.nextTick real-VM ordering, AbortSignal timeouts, Bun.sleep.
//   These assert on spawned bun processes / GC / unref semantics tied to a live
//   JSGlobalObject and OS clock, out of scope for the S0 scheduling core.
import std;
import mbun.jsc.event_loop;

namespace {

int gFailed = 0;

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

template <class A, class B>
void expect_eq(const A& a, const B& b, std::string_view what) {
    if (!(a == b)) {
        ++gFailed;
        std::println("  FAIL: {} (got {}, want {})", what, a, b);
    }
}

using mbun::jsc::event_loop::EventLoop;

// ── task FIFO order ─────────────────────────────────────────────────────────
void test_task_fifo() {
    EventLoop loop;
    std::vector<int> log;
    for (int i : {1, 2, 3}) {
        loop.enqueue_task([&log, i] { log.push_back(i); });
    }
    expect_eq(loop.pending_tasks(), 3u, "3 tasks queued");
    while (loop.run_next_task()) {}
    expect(log == std::vector<int>{1, 2, 3}, "tasks run in FIFO order");
    expect(!loop.has_pending(), "loop empty after draining tasks");
}

// ── microtask drained after each task ───────────────────────────────────────
void test_microtask_after_task() {
    EventLoop loop;
    std::vector<std::string> log;
    loop.enqueue_task([&] {
        log.emplace_back("task-a");
        loop.enqueue_microtask([&] { log.emplace_back("micro-a"); });
    });
    loop.enqueue_task([&] {
        log.emplace_back("task-b");
        loop.enqueue_microtask([&] { log.emplace_back("micro-b"); });
    });
    loop.tick();  // task-a then its microtask
    loop.tick();  // task-b then its microtask
    expect(log == std::vector<std::string>{"task-a", "micro-a", "task-b", "micro-b"},
           "microtasks drain after each task, not batched");
}

// ── process.nextTick strict priority over microtasks ────────────────────────
void test_next_tick_priority() {
    EventLoop loop;
    std::vector<std::string> log;
    loop.enqueue_microtask([&] { log.emplace_back("micro-1"); });
    loop.enqueue_next_tick([&] {
        log.emplace_back("nexttick-1");
        loop.enqueue_next_tick([&] { log.emplace_back("nexttick-2"); });
        loop.enqueue_microtask([&] { log.emplace_back("micro-2"); });
    });
    loop.drain_microtasks();
    // nextTick queue drains fully (incl. ones queued during it) before any
    // microtask; the microtask queued inside runs after nextTick empties.
    expect(log == std::vector<std::string>{"nexttick-1", "nexttick-2", "micro-1", "micro-2"},
           "nextTick has strict priority over microtasks");
}

// ── nested microtask ordering, ported from microtask.test.js ────────────────
// source: test/js/web/timers/microtask.test.js > it("queueMicrotask")
// Three top-level microtasks each nest a child; the canonical run order is
// 0..7. We assert every callback observes the expected sequence value.
void test_microtask_nesting_order() {
    EventLoop loop;
    int run = 0;
    std::vector<int> seen;
    auto observe = [&] { seen.push_back(run++); };

    loop.enqueue_microtask([&] {
        observe();  // 0
        loop.enqueue_microtask([&] { observe(); });  // 3
    });
    loop.enqueue_microtask([&] {
        observe();  // 1
        loop.enqueue_microtask([&] {
            observe();  // 4
            loop.enqueue_microtask([&] { observe(); });  // 6
        });
    });
    loop.enqueue_microtask([&] {
        observe();  // 2
        loop.enqueue_microtask([&] {
            observe();  // 5
            loop.enqueue_microtask([&] { observe(); });  // 7
        });
    });
    loop.drain_microtasks();
    expect(seen == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7},
           "nested queueMicrotask runs in FIFO nesting order (microtask.test.js)");
}

// ── timer deadline ordering ─────────────────────────────────────────────────
void test_timer_deadline_order() {
    EventLoop loop;
    std::vector<int> log;
    loop.set_timeout(20, [&] { log.push_back(20); });
    loop.set_timeout(5, [&] { log.push_back(5); });
    loop.set_timeout(10, [&] { log.push_back(10); });
    expect_eq(loop.pending_timers(), 3u, "3 timers scheduled");
    expect_eq(loop.next_timer_deadline_ns().value(), 5u * 1'000'000, "earliest deadline is 5ms");

    loop.advance_ms(100);
    const std::size_t fired = loop.run_due_timers();
    expect_eq(fired, 3u, "all 3 timers fired");
    expect(log == std::vector<int>{5, 10, 20}, "timers fire in deadline order");
    expect_eq(loop.pending_timers(), 0u, "heap empty after firing timeouts");
}

// ── equal deadline → insertion (FIFO) order via epoch tiebreak ──────────────
void test_timer_fifo_tiebreak() {
    EventLoop loop;
    std::vector<int> log;
    loop.set_timeout(10, [&] { log.push_back(1); });
    loop.set_timeout(10, [&] { log.push_back(2); });
    loop.set_timeout(10, [&] { log.push_back(3); });
    loop.advance_ms(10);
    loop.run_due_timers();
    expect(log == std::vector<int>{1, 2, 3}, "equal-deadline timers fire in insertion order");
}

// ── not-yet-due timers do not fire ──────────────────────────────────────────
void test_timer_not_due() {
    EventLoop loop;
    std::vector<int> log;
    loop.set_timeout(50, [&] { log.push_back(50); });
    loop.advance_ms(10);
    expect(!loop.has_due_timer(), "timer at 50ms not due at 10ms");
    expect_eq(loop.run_due_timers(), 0u, "nothing fires before deadline");
    loop.advance_ms(45);
    expect(loop.has_due_timer(), "timer due at 55ms");
    expect_eq(loop.run_due_timers(), 1u, "timer fires once past deadline");
    expect(log == std::vector<int>{50}, "delayed timer eventually fires");
}

// ── clearTimeout before firing ──────────────────────────────────────────────
void test_clear_timeout() {
    EventLoop loop;
    std::vector<int> log;
    loop.set_timeout(10, [&] { log.push_back(1); });
    const auto id2 = loop.set_timeout(10, [&] { log.push_back(2); });
    loop.set_timeout(10, [&] { log.push_back(3); });
    expect(loop.clear_timer(id2), "clear_timer returns true for live timer");
    expect(!loop.clear_timer(id2), "clearing an already-cleared timer returns false");
    expect_eq(loop.pending_timers(), 2u, "cleared timer removed from heap");
    loop.advance_ms(10);
    loop.run_due_timers();
    expect(log == std::vector<int>{1, 3}, "cleared timer does not fire");
}

// ── setInterval reschedules until cleared ───────────────────────────────────
void test_set_interval() {
    EventLoop loop;
    std::vector<std::uint64_t> ticks;
    std::int64_t id = 0;
    id = loop.set_interval(10, [&] {
        ticks.push_back(loop.now_ms());
        if (ticks.size() == 3) {
            loop.clear_timer(id);  // interval clears itself on the 3rd fire
        }
    });
    loop.run_until_idle();
    expect(ticks == std::vector<std::uint64_t>{10, 20, 30},
           "interval fires at 10,20,30 then self-clears");
    expect_eq(loop.pending_timers(), 0u, "no timers left after interval cleared");
    expect(!loop.has_pending(), "loop idle after self-clearing interval");
}

// ── one timer's callback clears another still-pending timer ─────────────────
void test_clear_in_callback() {
    EventLoop loop;
    std::vector<int> log;
    std::int64_t idB = 0;
    loop.set_timeout(10, [&] {
        log.push_back(1);
        loop.clear_timer(idB);  // cancel B, scheduled for the same instant
    });
    idB = loop.set_timeout(10, [&] { log.push_back(2); });
    loop.set_timeout(10, [&] { log.push_back(3); });
    loop.advance_ms(10);
    loop.run_due_timers();
    expect(log == std::vector<int>{1, 3}, "timer cleared mid-batch by earlier callback does not fire");
}

// ── run_until_idle: timers enqueue tasks + more timers ──────────────────────
void test_run_until_idle_mixed() {
    EventLoop loop;
    std::vector<std::string> log;
    loop.set_timeout(10, [&] {
        log.emplace_back("t10");
        loop.enqueue_microtask([&] { log.emplace_back("t10-micro"); });
        loop.set_timeout(5, [&] { log.emplace_back("t15-nested"); });
    });
    loop.set_timeout(20, [&] { log.emplace_back("t20"); });
    loop.enqueue_task([&] { log.emplace_back("task0"); });

    loop.run_until_idle();
    // task0 runs first (already ready at t=0), then timers advance the clock:
    // 10ms fires (+microtask, +nested 15ms), 15ms nested fires, 20ms fires.
    expect(log == std::vector<std::string>{"task0", "t10", "t10-micro", "t15-nested", "t20"},
           "run_until_idle interleaves tasks, microtasks, and virtual-time timers");
    expect(!loop.has_pending(), "loop fully drained");
}

}  // namespace

int main() {
    test_task_fifo();
    test_microtask_after_task();
    test_next_tick_priority();
    test_microtask_nesting_order();
    test_timer_deadline_order();
    test_timer_fifo_tiebreak();
    test_timer_not_due();
    test_clear_timeout();
    test_set_interval();
    test_clear_in_callback();
    test_run_until_idle_mixed();

    if (gFailed > 0) {
        std::println("test_event_loop: {} failed", gFailed);
        return 1;
    }
    std::println("test_event_loop: ok");
    return 0;
}
