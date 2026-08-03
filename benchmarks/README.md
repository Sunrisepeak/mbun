# benchmarks — 性能评估

对比三个实现在相同负载下的表现，在大节点（里程碑/重要模块完成）时更新：

| 代号 | 实现 | 获取方式 |
|---|---|---|
| `bun-zig` | bun 稳定版（Zig，v1.2.x 最后的 Zig 构建） | 官方 release 二进制 → `.mbun/bin/`（不入库） |
| `bun-rust` | bun canary / 新版（Rust，PR #30412 之后） | 官方 canary 二进制 → `.mbun/bin/`（不入库） |
| `mbun` | 本项目（MC++） | 成员内 `mcpp run bench-<模块>`（release，见下） |

> mbun 是 mcpp workspace。基准 bin 作为对应库成员的 target，从成员目录运行，例如：
> `cd modules/semver && mcpp run bench-semver -- ../../benchmarks/suites/semver 50`。
> ⚠️ 成员 `mcpp.toml` 必须含 `[build] default-profile = "release"`，否则 `mcpp run`
> 用 dev(-O0) 构建，测出的是未优化数字（见首份 semver 报告的教训）。

## 目录与命名

```
benchmarks/
├── README.md                       # 本文件（方法论）
├── tools/                           # 可复用采集辅助工具
├── suites/<模块>/                   # 基准负载脚本（可复现，入库）
└── reports/YYYYMMDD-<主题>.md       # 评估报告（含环境、数据、结论）
```

## 口径说明（semver/glob/toml/string_width 已升级为 A 级同口径）

**强制标准**：一切基准必须遵循 `docs/design/20260711-benchmark-standard.md`（口径分级 A/B、同口径要求、测量法、checksum 闸门、反作弊、bench-until-green）。

**T-bench-align 已落地**：mbun 现在能跑 JS——`mbun run <script.mjs>`（`app/cli` + `mbun.jsc.runtime`，经 JSC C API
把 `Bun.semver`/`Bun.Glob`/`Bun.TOML`/`Bun.stringWidth`/`Bun.nanoseconds`/`console.log`/`process.argv` 等绑定为
全局 JS API）。四份微基准（semver/glob/toml/string_width）已用**三方二进制跑逐字节相同的 `.mjs`** 重测为
**口径 A（同口径）**，checksum 三方逐位一致。

**A 级结论（诚实，T-opt.jsc-bindings 零拷贝绑定优化后）**：绑定层（`JSValueToStringCopy` 每参 malloc/转码 +
C-API 回调 `JSLock` 抓放）经 **JSString 零拷贝 view + native host function** 消除后，同口径下 mbun：
**semver 反超两版 bun（order 1.9× / satisfies 2.1×，此前落后 ~3×）**、**glob 追平 bun-rust（此前落后 2.5×）**、
**string_width 在 JSC UTF-16/Latin-1 直通后达到 28.39M ops/s，反超 bun-rust 6.7%、bun-zig 88%，三实现最优**；
**toml 未变（1.1–1.6× 落后，主成本是 JS 对象逐属性物化，需 native `putDirect`，登记后续）**。
旧 B 级 native 直调的「mbun 最优」曾是**假性的快**，已作废、仅留作内核下界附注。详见各报告。

测试同口径同理：待 T3.4 `bun:test` 运行器后，`mbun test compat/bun/test/<file>` 与 bun 同口径直跑。

## 方法论

1. **同负载同口径**：同一份输入数据/迭代次数；bun 侧用 `bun -e` 或 `bun run` driver，mbun 侧用等价 C++ driver 或 CLI；计时都取进程内热循环（排除启动），启动类基准单独测。
2. **重复取中位数**：每项 ≥10 轮取中位数 + 标准差；机器空载；报告记录 CPU 型号/内存/OS/编译器版本（不含本机路径与主机名等隐私）。
3. **对比维度**：吞吐（ops/s）、延迟、二进制体积、RSS 峰值、启动时间。
4. **性能纪律**（源自 bun bench-until-green 经验）：mbun 若慢于任一 bun 版本 → 建立优化任务（拆解到 `docs/plan/`），优化到不慢为止；每份报告给出「mbun 是否最佳」结论与差距。
5. 基准脚本入库（`suites/`），二进制与临时数据放 `.mbun/`（gitignore）。

## 剖析工具

性能热点/瓶颈追踪工具见 [tools/PROFILING.md](tools/PROFILING.md)：`bench3.sh`（同口径三实现对照）、`prof.hpp`（段计时热点）、`profile.sh`（perf/指引）、`run-native-bench.sh`（原生 bench）。

## 报告索引

- [20260710-semver.md](reports/20260710-semver.md) — 口径 A：三方同一 `.mjs`（**mbun 反超 1.9×/2.1×**，checksum 一致；T-opt.jsc-bindings ✅ 落地）
- [20260711-glob.md](reports/20260711-glob.md) — 口径 A：三方同一 `.mjs`（**mbun 追平 bun-rust**、微落后 bun-zig 1.1×，checksum 一致；T-opt.jsc-bindings ✅ 落地）
- [20260711-toml.md](reports/20260711-toml.md) — 口径 A：三方同一 `.mjs`，含 JS 对象构造（mbun 落后 1.1×/1.6×，**未变**——瓶颈是物化，需 native `putDirect`，登记后续）
- [20260711-string-width.md](reports/20260711-string-width.md) — 口径 A：三方同一 `.mjs`（**mbun 28.39M，反超 bun-rust 6.7% / bun-zig 88%**，checksum 一致；GCC16/LLVM22 均最优）
- [20260711-core-io.md](reports/20260711-core-io.md) — 定位 I/O、raw wrapper 开销与 caller buffer 对齐对照
- [20260711-config-jsonc-ini.md](reports/20260711-config-jsonc-ini.md) — ⚠️ 口径 B：JSONC/INI parser kernel、checksum、双工具链与 perf 热点；A 级 JSC/真实 npmrc consumer 待集成
- [20260711-url-search-params.md](reports/20260711-url-search-params.md) — 口径 A 候选：Bun 两版已测，mbun JSC binding 未接线；native 仅作内核优化参考

## 原生 bun bench 采集辅助

`tools/mitata-json-preload.js` 绕过 Bun 1.3.x `console.log` 对 mitata 大型 JSON
输出的截断，令 `BENCHMARK_RUNNER=1` 的原始 p50 样本可被 `jq` 完整读取。依赖与
二进制仍放 `.mbun/`，采集结果归档到 `compat/data/native-bench-runs.json`。
