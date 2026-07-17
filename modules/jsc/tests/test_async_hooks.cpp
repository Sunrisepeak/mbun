// Focused AsyncLocalStorage regressions. The behavior comes from Bun's
// src/js/node/async_hooks.ts context-frame model; these checks exercise the
// actual JS runtime and its callback-registration boundaries.
import std;
import mbun.jsc.runtime;

namespace {

int gFailed {0};

void expect_num(std::string_view source, double expected, std::string_view message) {
    auto result {mbun::jsc::runtime::eval_number(source)};
    if (!result || *result != expected) {
        ++gFailed;
        std::println("  FAIL: {} ({})", message,
                     result ? std::format("got {}", *result) : result.error());
    }
}

void expect_eval(std::string_view source, std::string_view message) {
    auto result {mbun::jsc::runtime::eval(source)};
    if (!result) {
        ++gFailed;
        std::println("  FAIL: {} ({})", message, result.error());
    }
}

}  // namespace

int main() {
    expect_num(
        R"JS((()=>{
          const {AsyncLocalStorage}=require("async_hooks");
          const a=new AsyncLocalStorage(), b=new AsyncLocalStorage();
          let seen="";
          a.run("a",()=>b.run("b",()=>{seen=a.getStore()+b.getStore()}));
          return seen==="ab"&&a.getStore()===undefined&&b.getStore()===undefined?1:0;
        })())JS",
        1.0, "nested stores restore independently");

    expect_num(
        R"JS((()=>{
          const {AsyncLocalStorage,AsyncResource}=require("async_hooks");
          const storage=new AsyncLocalStorage(); let resource;
          storage.run("captured",()=>{resource=new AsyncResource("focused")});
          return resource.runInAsyncScope(()=>storage.getStore())==="captured"?1:0;
        })())JS",
        1.0, "AsyncResource restores its construction snapshot");

    expect_num(
        R"JS((()=>{
          const {AsyncLocalStorage}=require("async_hooks");
          const storage=new AsyncLocalStorage(); let bound,snapshot;
          storage.run("static",()=>{
            bound=AsyncLocalStorage.bind((prefix)=>prefix+storage.getStore(),"x:");
            snapshot=AsyncLocalStorage.snapshot();
          });
          return bound()==="x:static"&&snapshot(()=>storage.getStore())==="static"?1:0;
        })())JS",
        1.0, "static bind and snapshot preserve the captured frame");

    expect_eval(
        R"JS((()=>{
          const {AsyncLocalStorage}=require("async_hooks");
          const {EventEmitter}=require("events");
          const storage=new AsyncLocalStorage();
          const emitter=new EventEmitter();
          globalThis.__asyncHooksSeen=[];
          let resolvePromise;
          const pending=new Promise((resolve)=>{resolvePromise=resolve});
          storage.run("value",()=>{
            pending.then(()=>__asyncHooksSeen.push("then:"+storage.getStore()));
            Promise.reject(1).catch(()=>__asyncHooksSeen.push("catch:"+storage.getStore()));
            Promise.resolve().finally(()=>__asyncHooksSeen.push("finally:"+storage.getStore()));
            queueMicrotask(()=>__asyncHooksSeen.push("microtask:"+storage.getStore()));
            process.nextTick(()=>__asyncHooksSeen.push("nextTick:"+storage.getStore()));
            setTimeout(()=>__asyncHooksSeen.push("timeout:"+storage.getStore()),0);
            setImmediate(()=>__asyncHooksSeen.push("immediate:"+storage.getStore()));
            emitter.on("data",()=>__asyncHooksSeen.push("event:"+storage.getStore()));
          });
          resolvePromise();
          emitter.emit("data");
        })())JS",
        "schedule focused async callback boundaries");
    expect_eval("0", "drain promise and microtask reactions");
    // Timers are real wall-clock now: a 0ms deadline is due immediately, so a
    // single drain fires it (the old 1ms delay made this drain racy).
    expect_eval("__mbun_drain_timers(20)", "drain due timers");
    expect_eval("0", "drain timer-created reactions");
    expect_num(
        R"JS((["then","catch","finally","microtask","nextTick","timeout","immediate","event"]
          .every((name)=>__asyncHooksSeen.includes(name+":value")))?1:0)JS",
        1.0, "promise, timer, nextTick, and EventEmitter callbacks retain context");

    expect_eval(
        R"JS((()=>{
          const {AsyncLocalStorage}=require("async_hooks");
          const storage=new AsyncLocalStorage();
          globalThis.__asyncHooksStream=[];
          storage.run("stream",()=>{
            globalThis.__focusedStream=new ReadableStream({
              pull(controller){
                __asyncHooksStream.push(storage.getStore());
                controller.enqueue("ok"); controller.close();
              }
            });
          });
          __focusedStream.getReader().read();
        })())JS",
        "schedule ReadableStream pull outside its creation frame");
    expect_eval("0", "drain ReadableStream pull reaction");
    expect_num("__asyncHooksStream[0]==='stream'?1:0", 1.0,
               "ReadableStream source callback retains creation context");

    expect_num(
        R"JS((()=>{
          const {AsyncLocalStorage}=require("async_hooks");
          const storage=new AsyncLocalStorage();
          storage.enterWith("entered"); storage.disable();
          if(storage.getStore()!==undefined)return 0;
          storage.enterWith("again"); return storage.getStore()==="again"?1:0;
        })())JS",
        1.0, "disable and enterWith follow Bun re-enable semantics");

    if (gFailed != 0) {
        std::println("test_async_hooks: {} failed", gFailed);
        return 1;
    }
    std::println("test_async_hooks: ok");
    return 0;
}
