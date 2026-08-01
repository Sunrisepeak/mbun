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
- **过滤本地敏感信息。** 把诊断信息写入代码注释、commit 元数据或 PR 标题/正文/评论前,
  必须移除或用通用占位符替换本地绝对路径、用户名、主机名、token/凭据、私有 URL、环境值和机器标识。
  发布复制的日志或错误片段前再次检查。
- **合并 PR 更新。** 不要为每个小探针或本地编辑都发评论。只在有实际源码改动、完成一轮有界验证、
  阻塞/策略发生变化或资源安全事件等大节点更新,并附量化证据和下一步方向; 相邻结果尽量合并为一条评论。
- **保证 commit 有实质内容。** 普通开发 commit 必须包含推进任务的源码或测试实现; 有需要时在同一 commit
  中补充 `changelog.md` 或交接文档。纯文档 commit 仅用于明确要求的规则/设计/交接变更或维护者要求,
  且必须说明用途,不能把每个中间探针都做成文档 commit。
- **使用有界并行轮次。** 用户指定每轮并行度时,独立的有界任务按该并行度执行(本任务当前为 5–8);
  若内存、swap 或磁盘安全要求降低并行度,必须记录调整原因。

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
