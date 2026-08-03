# mbun 性能剖析工具箱（可复用）

沉淀到项目里的性能热点/瓶颈追踪工具，配合 `docs/design/20260711-benchmark-standard.md` 使用。
目标：让每次性能优化都**数据驱动**（先测热点→再改→bench3 前后对照），而不是猜。

## 工具清单

| 工具 | 作用 | 是否需 root/perf |
|---|---|---|
| `bench3.sh <suite> [rounds] [core]` | **同口径 A 级三实现对照**：bun-zig/bun-rust/mbun 跑相同 bench.mjs，定核+多轮中位，checksum 三方一致闸门，输出 Markdown 表 | 否 |
| `prof.hpp` | **段计时热点剖析**：热路径 `#include` + `MBUN_PROF_SCOPE("名")` 埋点，退出打印各子操作 (总耗时/调用/占比) 排行 | 否 |
| `profile.sh <binary> [args]` | 智能剖析：perf 可用则 `perf record`+火焰图，否则给出段计时指引 | perf 需权限，否则退化 |
| `run-native-bench.sh <suite>` | 跑上游 `compat/bun/bench` 原生 suite（公共 npm 装 mitata 绕过私有 registry 401） | 否 |

## 典型流程（bench-until-green）

```bash
# 1. 同口径基准，看当前差距（哪项慢、慢多少、checksum 是否一致）
benchmarks/tools/bench3.sh semver 5

# 2. 定位热点（本环境 perf 被禁，用段计时）：在热路径埋点
#    modules/jsc/src/runtime.cppm 顶部：#define MBUN_PROF 1  #include ".../prof.hpp"
#    包住可疑子操作：{ MBUN_PROF_SCOPE("extract_string"); ... }
#    跑一次 mbun run bench.mjs，看 stderr 的段计时榜（谁占比最大就先优化谁）

# 3. 改完后再 bench3 对照，确认收敛/反超 + checksum 不变；更新报告为最新 A 级数据
benchmarks/tools/bench3.sh semver 5
```

## 启用 perf（可选，能力更强：CPU 采样/缓存/分支/火焰图）

本环境默认 `perf_event_paranoid=4`（禁 perf）。放开（需一次 sudo）：

```bash
# 临时（重启失效）
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
# 持久
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf && sudo sysctl --system
# 可选：内核符号
echo 0 | sudo tee /proc/sys/kernel/kptr_restrict
```

取值：`4`=全禁 → `2`=仅用户态采样 → `1`=用户+内核（剖析 mbun 最佳）→ `-1`=不限。

无全局改动的 no-root 备选（仍需一次 sudo 给 perf 二进制授能力）：
```bash
sudo setcap cap_perfmon,cap_sys_ptrace+ep "$(command -v perf)"
```
放开后 `profile.sh` 会自动切到 `perf record`。火焰图需 `stackcollapse-perf.pl`+`flamegraph.pl`（FlameGraph 仓库，未装可 clone 到 `.mbun/`）。

## 说明

- 段计时（`prof.hpp`）只测 CPU 墙钟占比，不测缓存/分支——那些要 perf；但"子操作占比"足以定位大多数热点（如 JSC 绑定的字符串 malloc vs 内核）。
- 剖析构建建议带符号/帧指针：临时给成员加 `[profile.release] debug = true` 或 `-fno-omit-frame-pointer`（perf DWARF 栈回溯用）。
- 所有工具无本机路径依赖，跨机可复现。

## 实战示例：perf 已启用后定位 mbun 绑定热点（2026-07-11）

`perf_event_paranoid` 放开到 1 后，`profile.sh` 自动用 `perf record`。对 `mbun run` semver 基准
采样（函数级 flat，仅列函数名与占比，无路径/隐私）：

| 占比 | 函数 | 含义 |
|---:|---|---|
| 52.9% | `val_to_string` → `JSValueToStringCopy` (48.2%) | **C API 字符串提取**（每参数 malloc+全量转码）——头号热点 |
| ~26% | `JSEvaluateScript`/`JSC::Interpreter::executeProgram` | JS 脚本执行本身（不可避免） |
| ~21% + ~19% | `JSC::JSLockHolder` 构造/析构、`grabAllLocks`/`DropAllLocks` | **每次绑定回调抓/放 JSLock**——C API 每调用丢锁重抓，bun 已移除 C API 锁 |
| ~14% | `JSC::StackManager::setStackSoftLimit` | 每调用重设栈软限（伴随 DropAllLocks） |

**结论**：mbun 同口径落后的两大根因都是 **JSC C API 绑定开销**，与内核算法无关——
① `JSValueToStringCopy` malloc/转码；② 每调用 JSLock grab/drop。两者都指向「改用 JSC C++
internals 零拷贝 + 免丢锁」（T-opt.jsc-bindings 方案，见 `docs/design/20260711-jsc-bindings-opt.md`）。
这印证了 perf 工具的价值：无需读源码猜，直接看到 48% 花在字符串复制上。

### 跨模块确认：绑定开销主导，内核不是瓶颈（2026-07-11）

再对 string_width（A 级最差，落后 6–10×）采样，结论一致且更明确（函数名+占比，无隐私）：

| 占比 | 函数 | 类别 |
|---:|---|---|
| 39.4% | `val_to_string`→`JSValueToStringCopy`(36.4%) | 绑定：字符串提取 |
| ~60%(累计) | `JSLock` DropAllLocks/lock/unlock/didAcquire/willRelease… | 绑定：每调用丢锁重抓 |
| 7.6% + 6.5% | `JSStringRelease` / `to_utf8` | 绑定：字符串释放/转码 |
| **8.9%** | `mbun::core::strings::string_width`（已优化内核） | **内核（仅 8.9%）** |

**跨模块结论**：semver 与 string_width 的 A 级落后都是 **JSC C API 绑定开销主导（~90%）**，
已优化的内核（string_width 31M native）在 JS 边界下只占 ~9%。即**单点优化 T-opt.jsc-bindings
（零拷贝 + 免丢锁）可一次性收敛全部四个模块的同口径落后**——内核算法不是瓶颈。这也说明：
先前 T-opt.strings-width 把内核提速 4.5× 是必要的（否则内核占比会更大），但当前瓶颈已完全转移到绑定层。
