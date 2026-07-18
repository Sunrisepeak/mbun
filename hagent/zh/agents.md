# Agents

[English](../agents.md) · 以英文本 [`../agents.md`](../agents.md) 为准。

面向任何在本仓工作的编码 agent 的工具中立操作入口。权威是 [宪法](charter.md);
参与基础见 [contributing.md](contributing.md)。本页是你的 checklist。

## 先读对的 skill

操作规程位于 [`.agents/skills/`](../../.agents/skills/)。动手**前**先读与任务匹配的
那一个,别重复摸索:

| 何时 | skill |
| --- | --- |
| 写/审 C++ 模块(命名、`.cppm`、模块布局) | [`mcpp-style-ref`](../../.agents/skills/mcpp-style-ref/SKILL.md) |
| 开发任何功能 / 修复任何缺陷 | [`tdd-workflow`](../../.agents/skills/tdd-workflow/SKILL.md) |
| 运行时测试跑不过 / 崩溃 / hang | [`mbun-runtime-debugging`](../../.agents/skills/mbun-runtime-debugging/SKILL.md) |

## 硬规则

- **可能 spawn/hang 的一律沙箱。** 经 `tools/integration/safe-test.sh` /
  `bounded_run.py` 跑,绝不裸跑。(一次 fork 风暴可冻机。)
- **`compat/` 只读。** 可做移植性适配(路径、runner 桥接),但绝不弱化断言语义。
- **无证据不得声称完成。** 构建通过 ≠ 功能完成。给出套件 before→after 计数 + 复现
  命令(见 [contributing.md](contributing.md#证据))。
- **conventional commits**,一 commit 一开发项;红绿分离提交更佳。
- **目标分支。** PR 提交到 **`rewrite_bun_in_mcpp`**,绝不合入 `main`。

## 每个 commit 都署名到构建者

你**永远是 co-author,绝不是主作者**。用
[contributing.md](contributing.md#commit-规范) 的格式:构建者 `Signed-off-by:` +
你的 `Co-authored-by:` 行(agent 名 + 模型;有官方 no-reply 邮箱就用,没有留空
`<>`)。你署名给的构建者必须能**读懂这份 diff**,所以保持它可审。

## 不自 merge 受保护面

`hagent/**`、`.agents/skills/**`、根指针、`.github/workflows/**`、`CODEOWNERS`、
`LICENSE`(宪法 §5)须**维护者签字**。开 PR,不要自己 merge。

## 经 changelog 协调

`changelog.md` 是共享工作日志与交接协议:

- 记实质进展并带证据(套件 `x/y → x'/y'`,零回归声称要有 before/after 对照)。
- 留足信息,让下一个 session/agent 无需重新摸索即可接手。
- 动手前先看它,避免与在途工作重复或冲突。
