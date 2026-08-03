// Regression test: Bun.spawn with stdin: "pipe" must ride the fully async
// spawnEx/__mbun_io_tick path — never the blocking pipe-drain path. A blocking
// drain parks the single JS thread, so the virtual event loop (net reactor,
// timers, microtasks) starves and a child that talks back to an in-process
// Bun.serve deadlocks until its I/O timeout (the bun-install.test.ts hang:
// dummy-registry Bun.serve × spawned `mbun install`).
#include <JavaScriptCore/JavaScript.h>

import std;
import mbun.jsc.runtime;

namespace {

int gFailures { 0 };

void expect_number(std::string_view source, double expected, std::string_view message) {
    const auto result { mbun::jsc::runtime::eval_number(source) };
    if (!result || *result != expected) {
        ++gFailures;
        std::println("FAIL: {} (got {})", message,
                     result ? std::to_string(*result) : std::string { "evaluation error" });
    }
}

// Drive the same virtual loop the CLI pump runs: timers, net reactor, child
// I/O tick, and a bare eval whose end-of-script drains the JSC microtask queue.
bool pump_until(std::string_view doneExpression, int maxTurns = 200'000) {
    for (int turn { 0 }; turn < maxTurns; ++turn) {
        const auto done { mbun::jsc::runtime::eval_number(doneExpression) };
        if (done && *done == 1.0) return true;

        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbun_drain_timers ? __mbun_drain_timers(8) : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbunNetDrain ? __mbunNetDrain() : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbun_io_tick ? __mbun_io_tick() : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number("0"));
    }
    return false;
}

bool proc_natives_available() {
    const auto available { mbun::jsc::runtime::eval_number(
        "globalThis.__mbunProcNative && __mbunProcNative.spawnEx ? 1 : 0") };
    return available && *available == 1.0;
}

// stdin:"pipe" spawn round trip: write → end → stdout echoes → exit 0, all
// resolved through the pump (a blocking drain would wedge before the await).
void test_stdin_pipe_round_trip() {
    static_cast<void>(mbun::jsc::runtime::eval_number(R"JS((()=>{
      globalThis.__pipeEcho = 0;
      (async () => {
        const p = Bun.spawn({ cmd: ["/bin/cat"], stdin: "pipe", stdout: "pipe", stderr: "pipe" });
        p.stdin.write("hello-pipe");
        p.stdin.end();
        const out = await p.stdout.text();
        const code = await p.exited;
        globalThis.__pipeEcho = (out === "hello-pipe" && code === 0) ? 1 : 2;
      })().catch(() => { globalThis.__pipeEcho = 3; });
      return 0;
    })())JS"));
    if (!pump_until("globalThis.__pipeEcho !== 0 ? 1 : 0")) {
        ++gFailures;
        std::println("FAIL: stdin-pipe spawn never settled (blocking drain regression)");
        return;
    }
    expect_number("globalThis.__pipeEcho", 1.0,
                  "Bun.spawn stdin:'pipe' echoes bytes through /bin/cat and exits 0");
}

// The bun-install hang shape: stdout of a stdin:"pipe" child is awaited while
// the child's completion depends on the virtual loop staying alive (a timer
// fires, an in-process Bun.serve fetch resolves, then stdin is closed). The old
// blocking drain wedged the JS thread at the await, so the timer/server never
// ran and the child never saw EOF.
void test_server_responsive_while_piped_child_alive() {
    static_cast<void>(mbun::jsc::runtime::eval_number(R"JS((()=>{
      globalThis.__serveAlive = 0;
      (async () => {
        const srv = Bun.serve({ port: 0, fetch: async () => { await Bun.sleep(2); return new Response("pong"); } });
        const p = Bun.spawn({ cmd: ["/bin/cat"], stdin: "pipe", stdout: "pipe" });
        let body = "";
        setTimeout(async () => {
          const res = await fetch("http://127.0.0.1:" + srv.port + "/");
          body = await res.text();
          p.stdin.write("x");
          p.stdin.end();
        }, 1);
        const out = await p.stdout.text();  // must NOT block the virtual loop
        const code = await p.exited;
        srv.stop();
        globalThis.__serveAlive = (body === "pong" && out === "x" && code === 0) ? 1 : 2;
      })().catch(() => { globalThis.__serveAlive = 3; });
      return 0;
    })())JS"));
    if (!pump_until("globalThis.__serveAlive !== 0 ? 1 : 0")) {
        ++gFailures;
        std::println("FAIL: in-process server starved while piped child was alive");
        return;
    }
    expect_number("globalThis.__serveAlive", 1.0,
                  "Bun.serve answers an in-process fetch while a stdin-pipe child runs");
}

}  // namespace

int main() {
    if (!proc_natives_available()) {
        std::println("spawn async pipe: natives unavailable on this platform, skipping");
        return 0;
    }
    test_stdin_pipe_round_trip();
    test_server_responsive_while_piped_child_alive();
    std::println("spawn async pipe: {} failures", gFailures);
    return gFailures == 0 ? 0 : 1;
}
