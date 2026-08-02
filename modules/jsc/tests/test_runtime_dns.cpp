// Focused regression coverage for Task 3 Fix Domain B: DNS flags, Node's
// callback-only contract, and worker completion through the JS-thread pump.
#include <JavaScriptCore/JavaScript.h>
#if !defined(_WIN32)
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

import std;
import mbun.jsc.runtime;

namespace {

int gFailed{0};
int gAllocationGcCalls{0};
#if !defined(_WIN32)
std::atomic<int> gFakeGetAddrInfoCalls{0};

int fake_getaddrinfo(const char* hostname, const char*, const addrinfo*, addrinfo** result) {
    ++gFakeGetAddrInfoCalls;
    if (std::string_view{hostname} != "dotted.hosts.test") return EAI_NONAME;
    static thread_local sockaddr_in address{};
    static thread_local addrinfo row{};
    address = {};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "203.0.113.9", &address.sin_addr);
    row = {};
    row.ai_family = AF_INET;
    row.ai_socktype = SOCK_STREAM;
    row.ai_addrlen = sizeof(address);
    row.ai_addr = reinterpret_cast<sockaddr*>(&address);
    *result = &row;
    return 0;
}

void fake_freeaddrinfo(addrinfo*) {}
#endif

void expect(bool condition, std::string_view what) {
    if (!condition) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

void collect_during_result_allocation(void* context) {
    ++gAllocationGcCalls;
    JSGarbageCollect(static_cast<JSContextRef>(context));
}

void expect_num(std::string_view source, double expected, std::string_view what) {
    auto result{mbun::jsc::runtime::eval_number(source)};
    if (!result || *result != expected) {
        ++gFailed;
        std::println("  FAIL: {} (got {})", what,
                     result ? std::to_string(*result) : std::string{"evaluation error"});
    }
}

bool pump_until(std::string_view doneExpression, int maxTurns = 200'000) {
    for (int turn{0}; turn < maxTurns; ++turn) {
        auto done{mbun::jsc::runtime::eval_number(doneExpression)};
        if (done && *done == 1.0) return true;

        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbun_drain_timers ? __mbun_drain_timers(1) : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbun_io_tick ? __mbun_io_tick() : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number("0"));
        std::this_thread::yield();
    }
    return false;
}

template <class Predicate>
bool spin_until(Predicate predicate, int maxTurns = 200'000) {
    for (int turn{0}; turn < maxTurns; ++turn) {
        if (predicate()) return true;
        std::this_thread::yield();
    }
    return false;
}

void test_native_completion_is_reentrant_safe() {
    constexpr int FANOUT{512};
    static_cast<void>(mbun::jsc::runtime::eval_number(std::format(
        "(()=>{{const native=globalThis.__mbunDnsNative;globalThis.__dnsReentrantDone=0;"
        "globalThis.__dnsReentrantOk=1;globalThis.__dnsReentrantSelfVisible=-1;"
        "native.lookup('127.0.0.1',4,0,(first)=>{{"
        "if(!Array.isArray(first)||first.length===0)__dnsReentrantOk=0;"
        "__dnsReentrantSelfVisible=native.drain();"
        "for(let i=0;i<{};i++)native.lookup('127.0.0.1',4,0,(rows)=>{{"
        "if(!Array.isArray(rows)||rows.length===0)__dnsReentrantOk=0;"
        "__dnsReentrantDone++}})}});return 0}})()",
        FANOUT)));

    if (!pump_until(std::format("globalThis.__dnsReentrantDone==={}?1:0", FANOUT))) {
        ++gFailed;
        std::println("  FAIL: reentrant native DNS fanout completed");
        return;
    }
    expect_num("globalThis.__dnsReentrantOk", 1.0,
               "native DNS drain survives callback-triggered callbacks_ rehash");
    expect_num("globalThis.__dnsReentrantSelfVisible", 0.0,
               "drain removes the current callback before reentrant JS");
}

void test_node_flags_translate_to_platform_ai_flags() {
    expect_num(
        "(()=>{const dns=require('node:dns');"
        "globalThis.__dnsFlagsDone=0;globalThis.__dnsFlagsOk=0;"
        "const report=(p)=>p.then(v=>({ok:true,v}),e=>({ok:false,e:String(e&&e.code)}));"
        "Promise.all(["
        "report(Bun.dns.lookup('127.0.0.1',{family:4,flags:dns.ADDRCONFIG})),"
        "report(dns.promises.lookup('127.0.0.1',{family:6,hints:dns.V4MAPPED})),"
        "report(dns.promises.lookup('127.0.0.1',{family:6,hints:dns.V4MAPPED|dns.ALL}))"
        "]).then(r=>{__dnsFlagsDetail=JSON.stringify(r);__dnsFlagsOk=(r[0].ok&&"
        "r[0].v.length>0&&r[1].ok&&r[1].v.family===6&&"
        "r[1].v.address.startsWith('::ffff:')&&r[2].ok&&r[2].v.family===6&&"
        "r[2].v.address.startsWith('::ffff:'))?1:0;__dnsFlagsDone=1});"
        "return dns.ADDRCONFIG===1024&&"
        "dns.V4MAPPED===2048&&dns.ALL===256?1:0})()",
        1.0, "node:dns exports the Node flag constants");
    if (!pump_until("globalThis.__dnsFlagsDone===1?1:0")) {
        ++gFailed;
        std::println("  FAIL: DNS flag lookup completed through the event-loop pump");
        return;
    }
    expect_num("globalThis.__dnsFlagsOk", 1.0,
               "Node flags are translated before entering POSIX getaddrinfo");
    auto ok{mbun::jsc::runtime::eval_number("globalThis.__dnsFlagsOk")};
    if (!ok || *ok != 1.0) {
        auto detail{mbun::jsc::runtime::eval_to_string("globalThis.__dnsFlagsDetail")};
        if (detail) std::println("  DNS flag detail: {}", *detail);
    }
}

void test_callback_apis_require_callbacks() {
    expect_num(
        "(()=>{const dns=require('node:dns');const cases=["
        "()=>dns.lookup('localhost'),()=>dns.resolve4('localhost'),"
        "()=>dns.reverse('127.0.0.1'),()=>dns.lookupService('127.0.0.1',80)];"
        "let n=0;for(const f of cases){try{f()}catch(e){if(e instanceof TypeError)n++}}"
        "return n})()",
        4.0, "callback-style node:dns APIs synchronously reject a missing callback");
}

void test_lookup_does_not_block_the_js_turn() {
    static_cast<void>(mbun::jsc::runtime::eval_number(
        "(()=>{const dns=require('node:dns');globalThis.__dnsOrder=[];"
        "dns.lookup('localhost',(err)=>{__dnsOrder.push(err?'error':'dns')});"
        "setTimeout(()=>__dnsOrder.push('timer'),0);return 0})()"));

    expect_num("globalThis.__dnsOrder.length===0?1:0", 1.0,
               "getaddrinfo completion is not run in the initiating JS turn");
    if (!pump_until("globalThis.__dnsOrder.includes('dns')?1:0")) {
        ++gFailed;
        std::println("  FAIL: asynchronous localhost lookup completed");
        return;
    }
    expect_num("globalThis.__dnsOrder[0]==='timer'&&globalThis.__dnsOrder[1]==='dns'?1:0",
               1.0, "a zero-delay timer progresses before worker DNS completion");
}

#if !defined(_WIN32)
void test_dotted_lookup_uses_system_resolver_backend() {
    namespace dnsTest = mbun::jsc::runtime::dns_testing;
    gFakeGetAddrInfoCalls = 0;
    dnsTest::set_getaddrinfo_backend(fake_getaddrinfo, fake_freeaddrinfo);
    static_cast<void>(mbun::jsc::runtime::eval_number(
        "(()=>{const dns=require('node:dns');globalThis.__dnsDottedDone=0;"
        "globalThis.__dnsDottedAddress='';dns.lookup('dotted.hosts.test',(err,address)=>{"
        "__dnsDottedAddress=err?String(err.code):address;__dnsDottedDone=1});return 0})()"));
    if (!pump_until("globalThis.__dnsDottedDone===1?1:0")) {
        ++gFailed;
        std::println("  FAIL: dotted lookup completed through fake system resolver");
    }
    expect_num("globalThis.__dnsDottedAddress==='203.0.113.9'?1:0", 1.0,
               "dotted lookup preserves getaddrinfo/NSS/hosts semantics");
    expect(gFakeGetAddrInfoCalls.load() == 1,
           "dotted lookup calls the injected getaddrinfo backend exactly once");
    dnsTest::reset_getaddrinfo_backend();
}
#endif

void test_delayed_backend_keeps_lookup_reverse_and_service_off_js_thread() {
    namespace dnsTest = mbun::jsc::runtime::dns_testing;
    const std::int64_t rootsBefore{dnsTest::root_balance()};
    void* context{dnsTest::js_context()};
    gAllocationGcCalls = 0;
    dnsTest::set_result_allocation_hook(collect_during_result_allocation, context);
    dnsTest::install_delayed_backend();

    static_cast<void>(mbun::jsc::runtime::eval_number(
        "(()=>{const dns=require('node:dns');globalThis.__dnsDelayedDone=0;"
        "globalThis.__dnsDelayedOk=1;globalThis.__dnsDelayedTimer=0;"
        "dns.lookup('held.test',(err,address)=>{if(err||typeof address!=='string')"
        "__dnsDelayedOk=0;__dnsDelayedDone++});"
        "dns.reverse('1.1.1.1',(err,names)=>{if(err||!Array.isArray(names)||!names.length)"
        "__dnsDelayedOk=0;__dnsDelayedDone++});"
        "dns.lookupService('1.1.1.1',443,(err,host,service)=>{"
        "if(err||typeof host!=='string'||typeof service!=='string')__dnsDelayedOk=0;"
        "__dnsDelayedDone++});setTimeout(()=>__dnsDelayedTimer=1,0);return 0})()"));

    if (!spin_until([] { return dnsTest::delayed_backend_entered(); })) {
        ++gFailed;
        std::println("  FAIL: delayed DNS backend was entered");
        dnsTest::release_delayed_backend();
        dnsTest::clear_delayed_backend();
        return;
    }
    JSGarbageCollect(static_cast<JSContextRef>(context));
    expect_num("globalThis.__dnsDelayedDone", 0.0,
               "held lookup/reverse/lookupService do not complete on the JS thread");
    if (!pump_until("globalThis.__dnsDelayedTimer===1?1:0")) {
        ++gFailed;
        std::println("  FAIL: JS timer did not fire while resolver backend was held");
    }
    expect_num("globalThis.__dnsDelayedTimer", 1.0,
               "JS timers remain responsive while resolver backend is held");
    expect_num("globalThis.__dnsDelayedDone", 0.0,
               "DNS completion occurs only after backend release");

    dnsTest::release_delayed_backend();
    if (!pump_until("globalThis.__dnsDelayedDone===3?1:0")) {
        ++gFailed;
        std::println("  FAIL: delayed DNS operations completed after release");
    }
    expect_num("globalThis.__dnsDelayedOk", 1.0,
               "delayed lookup/reverse/lookupService preserve result shapes");
    expect(dnsTest::root_balance() == rootsBefore,
           "DNS callback protect/unprotect balance returns to baseline");
    expect(gAllocationGcCalls > 0,
           "direct JSGarbageCollect hook ran between DNS result allocations");
    dnsTest::set_result_allocation_hook(nullptr, nullptr);
    dnsTest::clear_delayed_backend();
}

void test_dns_owner_shutdown_does_not_wait_for_inflight_libc() {
    namespace dnsTest = mbun::jsc::runtime::dns_testing;
    const std::int64_t rootsBefore{dnsTest::root_balance()};
    dnsTest::install_delayed_backend();
    static_cast<void>(mbun::jsc::runtime::eval_number(
        "globalThis.__dnsShutdownCallback=0;"
        "__mbunDnsNative.lookup('held-shutdown.test',4,0,()=>__dnsShutdownCallback++);0"));
    if (!spin_until([] { return dnsTest::delayed_backend_entered(); })) {
        ++gFailed;
        std::println("  FAIL: shutdown test backend was entered");
        dnsTest::release_delayed_backend();
        dnsTest::clear_delayed_backend();
        return;
    }

    dnsTest::destroy_async_state();
    expect(!dnsTest::delayed_backend_released(),
           "DNS owner destruction returns before held libc backend release");
    expect(dnsTest::root_balance() == rootsBefore,
           "DNS owner destruction clears and unprotects pending callbacks");
    expect_num("globalThis.__dnsShutdownCallback", 0.0,
               "destroyed DNS owner does not invoke callback");

    dnsTest::release_delayed_backend();
    expect(spin_until([] { return dnsTest::delayed_backend_exited(); }),
           "detached DNS worker exits after held backend returns");
    JSGarbageCollect(static_cast<JSContextRef>(dnsTest::js_context()));
    expect_num("globalThis.__dnsShutdownCallback", 0.0,
               "late worker completion cannot touch JSC after owner destruction");
    dnsTest::recreate_async_state();
    dnsTest::clear_delayed_backend();
}

void test_callbacks_and_results_survive_gc_until_js_thread_completion() {
    namespace dnsTest = mbun::jsc::runtime::dns_testing;
    constexpr int LOOKUPS{64};
    const std::int64_t rootsBefore{dnsTest::root_balance()};
    JSContextRef context{static_cast<JSContextRef>(dnsTest::js_context())};
    gAllocationGcCalls = 0;
    dnsTest::set_result_allocation_hook(
        collect_during_result_allocation,
        const_cast<void*>(static_cast<const void*>(context)));
    static_cast<void>(mbun::jsc::runtime::eval_number(std::format(
        "(()=>{{const dns=require('node:dns');globalThis.__dnsGcDone=0;"
        "globalThis.__dnsGcOk=1;for(let i=0;i<{};i++){{"
        "dns.lookup('localhost',{{all:true}},(err,rows)=>{{"
        "if(err||!Array.isArray(rows)||rows.length===0||"
        "typeof rows[0].address!=='string')__dnsGcOk=0;__dnsGcDone++}})}}return 0}})()",
        LOOKUPS)));

    for (int i{0}; i < 8; ++i) JSGarbageCollect(context);
    expect_num("globalThis.__dnsGcDone===0?1:0", 1.0,
               "GC-stressed DNS callbacks remain pending after submission");
    for (int turn{0}; turn < 200'000; ++turn) {
        auto done{mbun::jsc::runtime::eval_number(
            std::format("globalThis.__dnsGcDone==={}?1:0", LOOKUPS))};
        if (done && *done == 1.0) break;
        JSGarbageCollect(context);
        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbun_io_tick ? __mbun_io_tick() : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number("0"));
        std::this_thread::yield();
    }
    expect_num(std::format("globalThis.__dnsGcDone==={}?1:0", LOOKUPS), 1.0,
               "all protected DNS callbacks complete after repeated GC");
    expect_num("globalThis.__dnsGcOk", 1.0,
               "rooted DNS arrays and rows remain valid during completion allocation");
    expect(dnsTest::root_balance() == rootsBefore,
           "GC-stressed DNS protect/unprotect balance returns to baseline");
    expect(gAllocationGcCalls > 0,
           "direct JSGarbageCollect hook stressed rooted DNS arrays and rows");
    dnsTest::set_result_allocation_hook(nullptr, nullptr);
}

void test_record_queries_use_wire_backend_and_bun_shapes() {
    static_cast<void>(mbun::jsc::runtime::eval_number(
        "(()=>{const d=require('node:dns').promises;globalThis.__dnsRecordsDone=0;"
        "globalThis.__dnsRecordsOk=0;const checks=["
        "d.resolveSrv('_test._tcp.test.socketify.dev').then(x=>Array.isArray(x)&&x[0].name&&typeof x[0].port==='number'),"
        "d.resolveTxt('txt.socketify.dev').then(x=>Array.isArray(x)&&Array.isArray(x[0])&&x[0][0]==='bun_test;test'),"
        "d.resolveSoa('bun.sh').then(x=>x&&typeof x.serial==='number'&&typeof x.minttl==='number'),"
        "d.resolveNaptr('naptr.socketify.dev').then(x=>Array.isArray(x)&&x[0].flags==='S'&&x[0].order===1),"
        "d.resolveCaa('caa.socketify.dev').then(x=>Array.isArray(x)&&x[0].critical===0&&x[0].issue==='bun.sh'),"
        "d.resolveMx('bun.sh').then(x=>Array.isArray(x)&&typeof x[0].priority==='number'&&typeof x[0].exchange==='string'),"
        "d.resolveNs('bun.sh').then(x=>Array.isArray(x)&&typeof x[0]==='string'),"
        "d.resolvePtr('ptr.socketify.dev').then(x=>Array.isArray(x)&&x[0]==='bun.sh'),"
        "d.resolveCname('cname.socketify.dev').then(x=>Array.isArray(x)&&x[0]==='bun.sh'),"
        "d.resolve4('example.com',{ttl:true}).then(x=>Array.isArray(x)&&typeof x[0].address==='string'&&Number.isInteger(x[0].ttl)&&x[0].ttl>=0),"
        // Public resolvers commonly refuse ANY. A real DNS error still proves
        // the query reached the backend instead of the old fixed ENOTIMP stub.
        "d.resolveAny('_mbun-invalid.localhost').then(x=>Array.isArray(x)&&x.every(v=>typeof v.type==='string'),e=>e&&e.code!=='ENOTIMP')"
        "];Promise.all(checks).then(x=>{__dnsRecordsOk=x.every(Boolean)?1:0;__dnsRecordsDone=1},"
        "e=>{__dnsRecordsDetail=String(e&&e.stack||e);__dnsRecordsDone=1});return 0})()"));
    if (!pump_until("globalThis.__dnsRecordsDone===1?1:0", 2'000'000)) {
        ++gFailed;
        std::println("  FAIL: DNS record queries completed through the event-loop pump");
        return;
    }
    expect_num("globalThis.__dnsRecordsOk", 1.0,
               "record queries preserve Node callback/promise result shapes");
    auto ok{mbun::jsc::runtime::eval_number("globalThis.__dnsRecordsOk")};
    if (!ok || *ok != 1.0) {
        auto detail{mbun::jsc::runtime::eval_to_string("globalThis.__dnsRecordsDetail||''")};
        if (detail) std::println("  DNS record detail: {}", *detail);
    }
}

}  // namespace

int main() {
#if defined(_WIN32)
    std::println("test_runtime_dns: skipped (Windows DNS native backend is Wave2)");
    return 0;
#else
    test_native_completion_is_reentrant_safe();
    test_node_flags_translate_to_platform_ai_flags();
    test_callback_apis_require_callbacks();
    test_lookup_does_not_block_the_js_turn();
    test_dotted_lookup_uses_system_resolver_backend();
    test_delayed_backend_keeps_lookup_reverse_and_service_off_js_thread();
    test_dns_owner_shutdown_does_not_wait_for_inflight_libc();
    test_callbacks_and_results_survive_gc_until_js_thread_completion();
    test_record_queries_use_wire_backend_and_bun_shapes();

    if (gFailed != 0) {
        std::println("test_runtime_dns: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_dns: ok");
    return 0;
#endif
}
