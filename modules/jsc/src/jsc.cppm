// src/compat/jsc.cppm — module mbun.compat.jsc
// JavaScriptCore（bun fork 预编译产物 mbun.jsc-prebuilt）的 compat 封装层。
// 三层规则：上层（mbun.jsc.*）只准 import 本模块，不准直接 include JSC 头。
// 设计：docs/design/20260710-jsc-integration.md §3.2。
module;

// 全局模块片段：C API 头 + JSC::initialize 声明（C++ 符号，产物已导出；
// 放在全局片段以保持外部链接，避免模块 attach 改变符号归属）。
#include <JavaScriptCore/JavaScript.h>

namespace JSC {
void initialize();
}

export module mbun.compat.jsc;

import std;

namespace {

// JSStringRef → std::string（UTF-8），RAII 释放入参之外的临时资源由调用方负责。
std::string to_utf8(JSStringRef jsStr) {
    std::size_t cap { JSStringGetMaximumUTF8CStringSize(jsStr) };
    std::string buf(cap, '\0');
    std::size_t written { JSStringGetUTF8CString(jsStr, buf.data(), cap) };
    buf.resize(written > 0 ? written - 1 : 0);  // 去掉结尾 NUL
    return buf;
}

// 异常值 → 诊断字符串（"SyntaxError: ..." 等）。
std::string describe_exception(JSGlobalContextRef ctx, JSValueRef exception) {
    JSValueRef ignored { nullptr };
    JSStringRef jsStr { JSValueToStringCopy(ctx, exception, &ignored) };
    if (jsStr == nullptr) return "JSC: unknown exception";
    std::string msg { to_utf8(jsStr) };
    JSStringRelease(jsStr);
    return msg.empty() ? "JSC: unknown exception" : msg;
}

} // namespace

export namespace mbun::compat::jsc {

// 进程级 JS 虚拟机单例。
//
// ⚠️ 生命周期约束（实测，设计文档 §2.4）：bun 的 WebKit fork 移除了 C API
// 内部锁（bundler 自管锁），JSGlobalContextRelease 会触发 AtomStringImpl
// 断言 abort。因此 context 创建后进程级常驻、绝不 release（bun 自身即此
// 模型），进程退出由 OS 统一回收。
class Vm {
private:
    JSGlobalContextRef ctx_ { nullptr };

public:  // Big Five — 进程级单例，禁止拷贝/移动
    Vm(const Vm&)            = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&)                 = delete;
    Vm& operator=(Vm&&)      = delete;

public:
    static Vm& instance() {
        static Vm gVm;
        return gVm;
    }

    // 求值 JS 源码，结果按 number 语义返回（T3.1a 冒烟够用；富类型 Value
    // 封装随 T3.2+ 需求扩展）。语法错误/运行时异常 → unexpected(诊断信息)。
    std::expected<double, std::string> eval(std::string_view source) {
        std::string src { source };  // C API 需要 NUL 结尾
        JSStringRef script { JSStringCreateWithUTF8CString(src.c_str()) };
        JSValueRef exception { nullptr };
        JSValueRef result { JSEvaluateScript(ctx_, script, nullptr, nullptr, 1, &exception) };
        JSStringRelease(script);
        if (exception != nullptr) {
            return std::unexpected(describe_exception(ctx_, exception));
        }
        double num { JSValueToNumber(ctx_, result, &exception) };
        if (exception != nullptr) {
            return std::unexpected(describe_exception(ctx_, exception));
        }
        return num;
    }

    // Evaluate JS source and coerce the completion value to a UTF-8 string
    // (String(result) semantics). Upper layers (mbun.jsc.test_runner) use this
    // to read back rich JS state — formatted report text / JSON-encoded values —
    // that does not fit the number channel of eval(). A thrown exception or a
    // failed string coercion surfaces as unexpected(诊断信息).
    std::expected<std::string, std::string> eval_to_string(std::string_view source) {
        std::string src { source };  // C API needs NUL termination
        JSStringRef script { JSStringCreateWithUTF8CString(src.c_str()) };
        JSValueRef exception { nullptr };
        JSValueRef result { JSEvaluateScript(ctx_, script, nullptr, nullptr, 1, &exception) };
        JSStringRelease(script);
        if (exception != nullptr) {
            return std::unexpected(describe_exception(ctx_, exception));
        }
        JSStringRef jsStr { JSValueToStringCopy(ctx_, result, &exception) };
        if (exception != nullptr || jsStr == nullptr) {
            return std::unexpected(describe_exception(ctx_, exception));
        }
        std::string out { to_utf8(jsStr) };
        JSStringRelease(jsStr);
        return out;
    }

private:
    Vm() {
        JSC::initialize();  // bun 同款显式初始化（幂等）
        ctx_ = JSGlobalContextCreate(nullptr);
    }

    // 析构不调用 JSGlobalContextRelease —— 见类注释：fork 移除 C API 锁后
    // release 路径必 abort；context 与进程同生命周期。
    ~Vm() = default;
};

// 便捷入口：进程级单例上的求值。
std::expected<double, std::string> eval(std::string_view source) {
    return Vm::instance().eval(source);
}

// 便捷入口：求值并把结果按 String(result) 语义取回 UTF-8 字符串。
std::expected<std::string, std::string> eval_to_string(std::string_view source) {
    return Vm::instance().eval_to_string(source);
}

} // namespace mbun::compat::jsc
