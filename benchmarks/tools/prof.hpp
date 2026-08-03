// benchmarks/tools/prof.hpp — 轻量可复用「段计时」热点剖析器（无需 perf/root/valgrind）。
//
// 用途：当 perf/valgrind 不可用（本项目环境 perf_event_paranoid=4），用手动埋点定位
// 某热路径里**哪个子操作**吃时间——例如 JSC 绑定里「字符串提取 vs 内核计算 vs JS 对象
// 物化」各占多少。RAII 段计时，按名字聚合 (总耗时/调用数/占比)，退出时打印排序榜。
//
// 用法：
//   #define MBUN_PROF 1                 // 开启（不定义则所有埋点零开销、被编译器消除）
//   #include "prof.hpp"
//   void hot() {
//     MBUN_PROF_SCOPE("extract_args");  // 作用域计时
//     ... ;
//     { MBUN_PROF_SCOPE("kernel"); compute(); }
//   }
//   // main 结束时自动打印，或手动 mbun::prof::dump();
//
// 说明：用 steady_clock；线程局部累加避免锁开销，dump 时汇总。仅测 CPU 墙钟占比，
// 不测缓存/分支（那些需 perf）。适合"子操作占比"定位，配合 bench3.sh 做前后对照。
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <vector>

namespace mbun::prof {

struct Counter {
    std::string_view name;
    std::atomic<std::uint64_t> ns{0};
    std::atomic<std::uint64_t> calls{0};
};

inline std::vector<Counter*>& registry() {
    static std::vector<Counter*> r;
    return r;
}
inline std::mutex& reg_mutex() {
    static std::mutex m;
    return m;
}

inline Counter* make_counter(std::string_view name) {
    auto* c = new Counter{name};
    std::lock_guard lk{reg_mutex()};
    registry().push_back(c);
    return c;
}

class Scope {
public:
    explicit Scope(Counter* c) : c_{c}, t0_{std::chrono::steady_clock::now()} {}
    ~Scope() {
        auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0_)
                      .count();
        c_->ns.fetch_add(static_cast<std::uint64_t>(dt), std::memory_order_relaxed);
        c_->calls.fetch_add(1, std::memory_order_relaxed);
    }

private:
    Counter* c_;
    std::chrono::steady_clock::time_point t0_;
};

// Print a sorted breakdown (highest total time first).
inline void dump(std::FILE* out = stderr) {
    std::lock_guard lk{reg_mutex()};
    auto& r = registry();
    if (r.empty()) {
        return;
    }
    std::uint64_t total = 0;
    for (auto* c : r) {
        total += c->ns.load();
    }
    std::sort(r.begin(), r.end(),
              [](Counter* a, Counter* b) { return a->ns.load() > b->ns.load(); });
    std::fprintf(out, "\n=== mbun::prof 段计时热点榜 (总测量 %.3f ms) ===\n",
                 static_cast<double>(total) / 1e6);
    std::fprintf(out, "%-28s %12s %12s %10s %8s\n", "section", "total(ms)", "calls", "ns/call", "%");
    for (auto* c : r) {
        auto ns = c->ns.load();
        auto calls = c->calls.load();
        std::fprintf(out, "%-28s %12.3f %12llu %10llu %7.1f%%\n",
                     std::string{c->name}.c_str(), static_cast<double>(ns) / 1e6,
                     static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(calls ? ns / calls : 0),
                     total ? 100.0 * static_cast<double>(ns) / static_cast<double>(total) : 0.0);
    }
    std::fflush(out);
}

// Auto-dump at exit.
struct AtExit {
    ~AtExit() { dump(); }
};

}  // namespace mbun::prof

#if defined(MBUN_PROF) && MBUN_PROF
#define MBUN_PROF_CONCAT_(a, b) a##b
#define MBUN_PROF_CONCAT(a, b) MBUN_PROF_CONCAT_(a, b)
#define MBUN_PROF_SCOPE(name)                                                        \
    static ::mbun::prof::Counter* MBUN_PROF_CONCAT(mbun_prof_c_, __LINE__) =         \
        ::mbun::prof::make_counter(name);                                            \
    ::mbun::prof::Scope MBUN_PROF_CONCAT(mbun_prof_s_,                               \
                                         __LINE__){MBUN_PROF_CONCAT(mbun_prof_c_, __LINE__)}
#define MBUN_PROF_AT_EXIT() static ::mbun::prof::AtExit mbun_prof_atexit_{}
#else
#define MBUN_PROF_SCOPE(name) ((void)0)
#define MBUN_PROF_AT_EXIT() ((void)0)
#endif
