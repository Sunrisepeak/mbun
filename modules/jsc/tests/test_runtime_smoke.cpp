// T-bench-align smoke test — mbun.jsc.runtime exposes mbun modules as Bun.* JS
// API (shared by `mbun run` and the bun:test runner). Acceptance (task §4):
// eval "Bun.semver.order('1.0.0','1.0.1')" == -1, plus more bindings to guard
// the JS→native path end-to-end.
import std;
import mbun.jsc.runtime;

extern "C" void mbun_napi_test_dispatch_finalizer_error(void* context, const char* message);

namespace {

int gFailed{0};

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

void expect_num(std::string_view src, double want) {
    auto r{mbun::jsc::runtime::eval_number(src)};
    expect(r.has_value(), std::format("eval `{}` ok", src));
    if (r) expect(*r == want, std::format("eval `{}` == {} (got {})", src, want, *r));
}

}  // namespace

int main() {
    // Milestone acceptance.
    expect_num("Bun.semver.order('1.0.0','1.0.1')", -1.0);

    // semver satisfies, stringWidth, glob, TOML → real JS object, nanoseconds.
    expect_num("Bun.semver.satisfies('1.2.3','^1.0.0') ? 1 : 0", 1.0);
    expect_num("Bun.semver.order('2.0.0','1.0.0')", 1.0);
    expect_num("Bun.stringWidth('你好')", 4.0);
    expect_num("Bun.stringWidth('hello')", 5.0);
    expect_num("Bun.stringWidth(undefined)", 0.0);
    expect_num("Bun.stringWidth('\\x1b[31m', {countAnsiEscapeCodes:true})", 4.0);
    expect_num("Bun.stringWidth('“', {ambiguousIsNarrow:false})", 2.0);
    expect_num("Bun.stringWidth('§', {ambiguousIsNarrow:false})", 1.0);
    expect_num("(()=>{Object.prototype.countAnsiEscapeCodes=true;"
               "const n=Bun.stringWidth('\\x1b[31m',{});"
               "delete Object.prototype.countAnsiEscapeCodes;return n})()",
               0.0);
    expect_num("Bun.stringWidth('\\x1b[31m',"
               "Object.create({countAnsiEscapeCodes:true}))",
               4.0);
    expect_num("(()=>{try{Bun.stringWidth({toString(){throw 42}})}"
               "catch(e){return e===42?1:0}return 0})()",
               1.0);
    expect_num("(()=>{try{Bun.stringWidth(Symbol('x'))}"
               "catch(e){return e instanceof TypeError?1:0}return 0})()",
               1.0);
    expect_num("new Bun.Glob('a/**/c/*.md').match('a/bb/c/x.md') ? 1 : 0", 1.0);
    expect_num("new Bun.Glob('*.ts').match('x.js') ? 1 : 0", 0.0);
    expect_num("Object.keys(Bun.TOML.parse('a = 1\\nb = 2\\n[t]\\nx = 3')).length", 3.0);
    expect_num("Bun.TOML.parse('a = 41').a + 1", 42.0);
    expect_num("typeof Bun.nanoseconds() === 'number' ? 1 : 0", 1.0);

#if !defined(_WIN32)
    // The native finalizer drain has already removed the VM/env exception when
    // it reaches the process uncaught dispatcher. A broken/missing dispatcher
    // must therefore arm the shared fatal channel itself rather than drop the
    // finalizer error. A healthy dispatcher still receives it exactly once.
    void* napiContext{mbun::jsc::runtime::dns_testing::js_context()};
    expect_num("(()=>{globalThis.__mbun_fatal=null;globalThis.__mbun_fatal_status=1;"
               "globalThis.__mbun_uncaught=null;return 1})()",
               1.0);
    mbun_napi_test_dispatch_finalizer_error(napiContext, "missing dispatcher");
    expect_num("globalThis.__mbun_fatal?.[0]?.message==='missing dispatcher'&&"
               "globalThis.__mbun_fatal_status===1?1:0",
               1.0);

    expect_num("(()=>{globalThis.__mbun_fatal=null;globalThis.__mbun_fatal_status=1;"
               "globalThis.__mbun_uncaught=()=>{throw new Error('dispatcher failed')};return 1})()",
               1.0);
    mbun_napi_test_dispatch_finalizer_error(napiContext, "original finalizer error");
    expect_num("globalThis.__mbun_fatal?.[0]?.message==='dispatcher failed'&&"
               "globalThis.__mbun_fatal_status===7?1:0",
               1.0);

    expect_num("(()=>{globalThis.__mbun_fatal=null;globalThis.__mbun_fatal_status=1;"
               "globalThis.__mbunFinalizerDispatchCalls=0;"
               "globalThis.__mbun_uncaught=()=>{globalThis.__mbunFinalizerDispatchCalls++;"
               "return true};return 1})()",
               1.0);
    mbun_napi_test_dispatch_finalizer_error(napiContext, "handled finalizer error");
    expect_num("globalThis.__mbunFinalizerDispatchCalls===1&&"
               "globalThis.__mbun_fatal===null?1:0",
               1.0);
#endif

    if (gFailed > 0) {
        std::println("test_runtime_smoke: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_smoke: ok");
    return 0;
}
