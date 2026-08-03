---
name: dev-process
description: 在本仓开发任何改动前用。规定 issue 先行的开发流程,按 bugfix / 优化 / 新功能分流;新功能须经 issue 充分讨论并在 .agents/docs 落地设计方案;衔接 tdd-workflow 与本体验证 / 测试 / CI / PR 规范。开发前先读本 skill。
---

# dev-process —— 开发规范(issue 先行)

红绿实现细节见 [`tdd-workflow`](../tdd-workflow/SKILL.md);本 skill 管**它之前**的
流程:先有 issue、按类型分流、新功能先出设计方案。

## 1. 先建 issue,按类型分流

除极小修正(typo / 注释)外,改动都从一个 issue 开始。issue 写法见
[`issue-reporting`](../issue-reporting/SKILL.md)。

| 类型 | issue 要求 | 进入开发 |
| --- | --- | --- |
| **bugfix** | 复现 + 根因判断 | 直接进 tdd-workflow:red 用上游/新测试锚定,green 最小实现 |
| **优化 / 重构** | 动机 + **不改变语义**声明 | 需基线对照(改动前后 pass/fail 一致或更好) |
| **新功能** | 见 §2 充分讨论 + 设计方案 | 方案通过后再进 tdd-workflow |

## 2. 新功能:讨论 → 设计方案

- 在 issue 里**充分讨论**:动机、API 形态、影响面、与 bun/node 的兼容性、替代方案。
- 讨论通过后,在 [`.agents/docs/`](../../docs/) 新建 `<topic>.md` 设计方案,含:
  **目标 / 模块边界 / 接口设计 / 测试计划 / 兼容性与风险**。
- 优先落为独立 workspace 成员 + `.cppm` 模块(编码规范见
  [`mcpp-style-ref`](../mcpp-style-ref/SKILL.md))。

## 3. 本体验证(不是只构建)

改动必须在**真实 mbun** 上跑通对应上游测试——构建通过 ≠ 功能完成。选最新 mtime 的
二进制、经沙箱跑,见 [`mbun-runtime-debugging`](../mbun-runtime-debugging/SKILL.md)。

## 4. 测试

- 纯逻辑模块:落 `modules/<member>/tests/test_<模块>.cpp`(成员目录内 `mcpp test`
  自动发现)。
- 运行时行为:以 `mbun test compat/...` 的 pass/fail 为红绿信号。
- 一律经 `tools/integration/`(`safe-test.sh` / `bounded_run.py`)资源受限沙箱,防
  fork/泄漏冻机。

## 5. CI / PR

- **目标分支**:PR 提交到 **`rewrite_bun_in_mcpp`**,**不要合入 `main`**。
- CI:双 lane(见 `.github/workflows`);改动不得破坏受保护面。
- PR:用模板;附**证据**(套件 x/y → x'/y' + 复现命令);**署名**(构建者主签 +
  agent `Co-authored-by`);打**标签**。规范见
  [`hagent/contributing.md`](../../../hagent/contributing.md)。
- 触及**受保护面**(`hagent/**`、`.agents/skills/**`、CI、`LICENSE`;见
  [`hagent/charter.md`](../../../hagent/charter.md) §5)须**维护者签字**,不得自 merge。

## 架构原则:通用内核 + 各方言的薄兼容层

mbun 是**一个通用运行时内核**,外面套 **node / bun 两层薄兼容层**;方言**自动识别**,
并允许用户**显式指定**覆盖。

**推论(容易踩错的那条):两个语料对同一调用要求不同行为时,那是兼容层的分派点,
是工作项,不是不可达的上限。** 已确认至少 6 处(crypto 解码错误码、`getCurves()`、
HTTP/1.0 framing、入口找不到的报错、OKP JWK 校验、`getRandomValues` 配额)。
不要记成 `COSTS_GREEN` 了事。

落地要点:

- **先归类再分派**:进程级(argv0 / 入口扩展名 / 显式 flag、env)/ 比进程更细 /
  真正不可约。
- **分派只增加分支,不改默认路径**;每处都要**双语料门禁**,且比对**断言数**而非只看
  绿文件数 —— 共享面改动会在文件分类不变的情况下移动断言。
- **分派点写在 C++**,进程级解析一次供 JS 查询,不在各 builtin 的 JS payload 里各自嗅探
  (见 [`mcpp-style-ref`](../mcpp-style-ref/SKILL.md) 的「JS 只做最薄接口层」)。
- 现状:**该机制尚不存在**,动手前先看 `modules/jsc/src/runtime/api_impl.inc`,
  别假设它已经有了。

## 禁止

- ❌ 把双语料行为冲突当成天花板 / 记为不可达,而不是做成兼容层分派。
- ❌ 无 issue 直接开新功能;新功能跳过讨论 / 设计方案。
- ❌ 只构建就声称完成(必须本体验证)。
- ❌ 为通过测试弱化 `compat/` 上游断言语义。
- ❌ 一次 commit 混多个不相关开发项。
