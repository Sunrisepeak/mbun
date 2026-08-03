# hagent —— AI Agent 开源协作规范

[English](../README.md) · 以英文本 [`../README.md`](../README.md) 为准。

> **hagent** = **h**uman + **agent**。一套精简、可复用的 AI 时代开源协作规范。
> 诞生于 [mbun](../../README.zh-CN.md) 的目标 #3 实验(「验证与探索 AI-agent 驱动的
> 开发与协作」),并首先在本仓 dogfood。设计上可将来抽离成独立仓库。

> [!NOTE]
> **状态:experimental。** 模块 ① 宪法、② 入口文档、④ 标签已落地;③、⑤、⑥(受保护面
> 强制、权限自动化、CI 闸门)在 [charter.md](charter.md) 里作为路线图点名,后续迭代
> 落地。

## 核心命题

**分工按工作的「性质」,不按数量。**

- **Agent = 执行引擎** —— 实现、测试、PR 审查、常规改动的 merge。这占吞吐的
  90%+,于是自然落到 agent 身上。
- **构建者 = 负责创作与判断的人** —— **架构、决策、品位、规范**,以及少数特殊的细节
  设计与优化。构建者「参与构建与创作」,而非单纯守门。
- **90%+ 是结果,不是定义。** 执行占了体量绝大部分,所以绝大部分体量归 agent。

## 另外两条原则

- **所有操作归属到构建者。** 即使由 agent 执行,也以构建者的身份提交(构建者是主作者
  与主签名);agent 作为 **co-author** 记录其模型,且构建者必须读懂这份 diff。commit
  规范见 [contributing.md](contributing.md)。
- **证据先于「完成」。** 没有可复现证据(套件 before→after 计数 + 复现方法),不接受
  「通过 / 绿 / 零回归」的声称。见 [contributing.md](contributing.md) 与
  [PR 模板](../../.github/PULL_REQUEST_TEMPLATE.md)。

## 目录

| 文件 | 面向 | 是什么 |
| --- | --- | --- |
| [charter.md](charter.md) | 所有人 | 宪法:角色、执行-创作分工、归属、权限层级、受保护面、贡献生命周期、路线图。 |
| [contributing.md](contributing.md) | 对外(构建者 + 其 agent) | 如何参与:setup、何谓好贡献、commit 与署名规范、证据、沙箱底线。 |
| [agents.md](agents.md) | 对内(任何编码 agent) | 工具中立的操作入口:何时读哪个 skill、硬规则、经 changelog 协调、如何署名。 |
| [labels.md](labels.md) | 所有人 | 双语标签集(`中文 \| English`)与各自何时打。 |

根目录指针 `CONTRIBUTING.md`、`AGENTS.md`、`CLAUDE.md`、`GOVERNANCE.md` 转发到此,
供工具与 GitHub 自动发现。

agent 操作规程位于 mbun 的 [`.agents/skills/`](../../.agents/skills/)
(`mcpp-style-ref`、`tdd-workflow`、`mbun-runtime-debugging`);[agents.md](agents.md)
负责导航。
