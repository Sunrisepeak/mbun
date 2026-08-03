---
name: mbun-runtime-debugging
description: 调试 mbun 运行时测试失败/崩溃时用。提供仓库实测可用的定位配方——选二进制、单文件跑测、gdb 抓 backtrace、按 API 定位源码、常见坑排查清单、何时跳过。凡处理 bun 测试跑不过/段错误/hang/断言失败，先读本 skill 再动手，避免每个 task 重复摸索拖慢。
---

# mbun-runtime-debugging

把「一个 bun 测试文件跑不过」快速定位到 mbun 源码里那一处的实测配方。目标：**最少时间/最少 token 定位，不重复摸索**。

## 0. 选二进制（永远第一步）

```bash
BIN=$(find target -name mbun -type f -printf "%T@ %p\n" | sort -rn | head -1 | cut -d' ' -f2)
```
必须按 **最新 mtime** 选：裸 `find | head` 会选到过期 fingerprint 目录或交叉编译（musl）产物。

## 1. 跑单个测试文件

```bash
"$BIN" test compat/bun/test/<path>.test.ts
```
- `mbun test` 支持 `-t` 名称过滤与多文件；语料测试以单文件粒度最稳。
- 需要 fixture 相对路径 / bunfig 的测试要从 `compat/bun` 目录启动（bun 自己的 CI 就在 repo root 跑）。
- **可能 spawn 子进程 / 可能 hang 的测试**（child_process/spawn/install/serve/socket）必须沙箱化，否则 fork 堆积可冻机：
  ```bash
  tools/integration/safe-test.sh 30 "$BIN" test compat/bun/test/<path>
  ```
- 批量/全量：`python3 tools/integration/bun_corpus_runner.py --bin "$BIN" --cwd compat/bun --discover compat/bun/test --sample-per-group 100000 --out target/integration/<run> --jobs 14 --timeout 30`（内建逐例 systemd 沙箱、私有 TMPDIR、磁盘水位闸门；node 语料用 `node_corpus_runner.py`）。用法细节见 `tools/integration/README.md` 与 `compat/README.md`。

## 2. 断言失败（非崩溃）——最快路径

测试输出直接给 `(pass)`/`(fail)` 逐条 + 失败断言的 expected/received。**先只读那一条失败**：
```bash
"$BIN" test compat/bun/test/<path> 2>&1 | grep -A8 -i "fail\|expect\|error" | head -40
```
拿到失败的 API 名 → 跳第 4 节定位源码。多数「近绿」文件（fail≤2）就是 1 个语义小缺口，不必通读整文件。

## 3. 崩溃 / 段错误 / abort —— gdb 抓 backtrace

release 二进制**带 debug_info、未 strip**，gdb 可直接符号化。配方（`debuginfod` 关掉避免卡交互）：
```bash
timeout 60 gdb -q -batch \
  -ex "set debuginfod enabled off" -ex "set pagination off" \
  -ex "run" -ex "bt" -ex "info threads" \
  --args "$BIN" test compat/bun/test/<path> 2>&1 | tail -40
```
- 看栈顶第一个 `mbun::` / `modules/...` 帧——那通常就是 bug 点。
- JSC 相关崩溃常见根因：host callback 期间无锁裸跑内部路径触发 GC 断言（common.inc 的 DropAllLocks 注释）、分区 IIFE 作用域坑（见第 5 节）。
- 只崩在某断言后：设断点 `-ex "b <func>"` 再 `run`/`bt`。

## 4. 按 API 定位源码

```bash
# JS 层绑定（Bun.*/node:*/Web API）几乎都在 modules/jsc/src
grep -rn "<apiName>" modules/jsc/src | head
# 纯逻辑子系统在对应 modules/<子系统>/src
grep -rln "<symbol>" modules/*/src | head
```
- 行为缺口要**对照 bun 参考源** `compat/bun/src/<子系统>`（不黑盒臆测）。
- jsc 绑定的注册点：`builtins/*.cppm`（JS 侧）+ `runtime/*.inc`（host 侧，经 `runtime.cppm` 聚合）。

## 5. 常见坑排查清单（命中即秒修）

- **分区 IIFE 作用域**：`image_closure` 之后的 jsc builtins 分区必须自包 IIFE 且 `const G = globalThis;`，否则绑定**静默失效**（bind 不上、非报错）。新绑定看不到就查这个。
- **host-fn 无锁路径**：`set_fn` 用 `JSNativeStdFunction` trampoline；JSC C-callback 内只用纯 C API，别裸跑会触发 GC 的内部路径。
- **target/ 里的 chmod-000 垃圾**：语料的权限测试若在 `target/integration/*/tmp` 留下不可读目录，mcpp 源扫描会被绊倒并产出**残缺依赖图**（表象是 `failed to read compiled module`）。`chmod -R u+rwx` 后删除即恢复；`bounded_run.force_rmtree` 已防复发。
- **模块重名**：工作区内两个 `.cppm` 声明同名模块会让根包构建静默乱序（曾发生 `mbun.platform` 冲突）。`grep -rh "^export module" src modules | sort | uniq -d` 查重。
- **绝不弱化断言**换绿——测试是验收 spec，改 `compat/bun/test/` 下任何文件都算作弊（该目录是只读上游输入）。

## 6. 构建经济学（省时间的关键）

- **纯逻辑模块**改动 → 成员目录内 `mcpp test`（快，只编该成员）。
- **jsc/根包**改动 → 根目录 `mcpp build`（重，全链路）。多 agent 并行时禁各自根构建（build storm + 坏 gcm 缓存）：要么交协调者统一构建，要么 worktree 隔离（独立 target）。
- 增量缓存偶发顺序 hiccup → `mcpp clean --bmi-cache && mcpp build --no-cache` 自愈。

## 7. 何时跳过（别钻牛角尖，省 token）

失败根因是**整个缺失子系统 / 真实 driver / 工具链**（mysql/postgres 驱动、napi addon 构建、真实 npm 包如 `tunnel`）时，**不要**在近绿清扫轮里硬啃——记一行「blocked on X」跳过，留给对应深挖轨。近绿清扫只吃「已加载已运行、差 1–3 断言」的文件。

## 8. 提交纪律

`git add <你改的文件>`，**禁 `git commit -a`**（共享树会扫走别 agent 的暂存改动）；push 前查全 `origin/main..HEAD`。message：`fix(<scope>): <subject>`；实质进展同步 `changelog.md`。
