// T3.1a JSC 集成冒烟测试 — bun-webkit 预编译 JavaScriptCore 经
// mbun.compat.jsc 封装后可求值 JS 表达式。
// 验收（docs/plan T3.1）：eval("1+1") == 2。
// 设计：docs/design/20260710-jsc-integration.md §3.2。
import std;
import mbun.compat.jsc;

namespace {

int gFailed = 0;

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

} // namespace

int main() {
    using mbun::compat::jsc::eval;

    // 里程碑验收冒烟：数值表达式求值
    {
        auto r = eval("1+1");
        expect(r.has_value(), "eval(\"1+1\") 求值成功");
        expect(r.has_value() && *r == 2.0, "eval(\"1+1\") == 2");
    }

    // 稍复杂路径：数组 + 箭头函数 + 内建方法（覆盖 parser/内建库链路）
    {
        auto r = eval("[1,2,3].reduce((a,b)=>a+b)");
        expect(r.has_value(), "eval(reduce) 求值成功");
        expect(r.has_value() && *r == 6.0, "eval(\"[1,2,3].reduce((a,b)=>a+b)\") == 6");
    }

    // 语法错误 → unexpected（错误信息非空，供上层诊断）
    {
        auto r = eval("1+");
        expect(!r.has_value(), "eval(\"1+\") 语法错误返回 unexpected");
        expect(!r.has_value() && !r.error().empty(), "语法错误信息非空");
    }

    // 运行时异常（未定义标识符）同样走 unexpected 通道
    {
        auto r = eval("notDefinedAnywhere123");
        expect(!r.has_value(), "未定义标识符返回 unexpected");
    }

    // 同一进程内多次求值复用进程级 context（fork 移除 C API 锁 → context 常驻）
    {
        auto r1 = eval("globalThis.__mbunSmoke = 40");
        auto r2 = eval("globalThis.__mbunSmoke + 2");
        expect(r1.has_value(), "全局赋值求值成功");
        expect(r2.has_value() && *r2 == 42.0, "跨调用共享同一 global context");
    }

    if (gFailed > 0) {
        std::println("test_jsc_smoke: {} failed", gFailed);
        return 1;
    }
    std::println("test_jsc_smoke: ok");
    return 0;
}
