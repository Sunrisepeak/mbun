# 贡献指南

[English](../contributing.md) · 以英文本 [`../contributing.md`](../contributing.md) 为准。

面向构建者以及他们带来的 agent。背后的模型见 [宪法](charter.md);本页是实操 how-to。
(「构建者」= 运行该 agent、并为结果负责的人。)

## Setup

按项目 [README](../../README.zh-CN.md#快速开始) 从源码构建并运行示例。agent 还应阅读
[agents.md](agents.md)。

## 何谓好贡献

- **一 PR 一开发项。** scope 收紧;不相关改动分开提。
- **目标分支。** PR 提交到 **`rewrite_bun_in_mcpp`** 分支——**不要**合入 `main`。
- **TDD。** 遵循 [`tdd-workflow`](../../.agents/skills/tdd-workflow/SKILL.md):
  red → green → refactor,以上游测试语料为验收信号。
- **conventional commits。** `type(scope): subject`,type 取
  `feat` / `fix` / `test` / `refactor` / `docs` / `chore` / `research`。
- **守住底线**(宪法 §7):`compat/` 只读、不弱化断言、可能 spawn/hang 的测试只经
  沙箱(`tools/integration/safe-test.sh`)。

## commit 规范

每个 commit —— 包括 agent 执行的工作 —— 都以**构建者为主作者与主签名**,**agent 为
co-author**:

```
feat(html_rewriter): support element.setAttribute on void elements

- #123

Void elements dropped attribute writes because the serializer skipped
their attribute list. Route them through the shared attribute path and
add the upstream fixture as a member test.

Signed-off-by: Sunrisepeak <speakshen@163.com>
Co-authored-by: Claude (Opus 4.8) <noreply@anthropic.com>
```

- **标题** —— conventional-commit subject。
- **相关 issue** —— 用 `- #<n>` 列出,一行一个;GitHub 自动链接并展开(合并时关闭用
  `Closes #<n>`)。
- **详细描述** —— 改了什么、为什么。
- **`Signed-off-by:`** —— 构建者。同时用 `git commit --author` / 配置使
  `Author` = 构建者的邮箱。
- **`Co-authored-by:`** —— agent 及其模型(GitHub 可渲染)。有官方 no-reply 邮箱就用,
  没有就留空(`<>`)。

常见 agent(把 `<model>` 换成实际模型):

| 工具 | `Co-authored-by:` 行 |
| --- | --- |
| Claude / Claude Code | `Co-authored-by: Claude (<model>) <noreply@anthropic.com>` |
| OpenAI Codex | `Co-authored-by: Codex (<model>) <>` |
| Cursor | `Co-authored-by: Cursor (<model>) <>` |
| OpenCode | `Co-authored-by: opencode (<model>) <>` |
| GitHub Copilot | `Co-authored-by: GitHub Copilot (<model>) <>` |

commit 签给的构建者**必须读懂这份 diff**(宪法 §3)——这是默认,不是额外步骤。

## 证据

任何声称测试/行为变化的 PR **必须**给出可复现证据(经
[PR 模板](../../.github/PULL_REQUEST_TEMPLATE.md)社会性强制,暂未 CI 强制):

- **套件:** `<file/group>` —— before `x/y` → after `x'/y'`
- **复现:** runner 命令,或指向 `compat/data` 的链接

构建通过**不是**行为变化的证据。

## 获取权限

- **Triager 分诊** —— 账号有 **≥ 3 个已合入 PR** 即自助获得(含 agent):分诊、打
  标签、确认 bug、验证 issue/PR、跑 CI。
- **Committer 提交者**(写)—— 相关模块背景 + 重要贡献,须**维护者批准**。
- 更高层级(模块 / 项目维护者)仍在完善。

见宪法 [§4](charter.md#4-权限层级)。

## 审查与 merge

- agent 审查是默认路径;非受保护面的绿 PR 可由 agent 审查者 merge。
- 触及**受保护面**(宪法 §5)或高风险的 PR 须**维护者签字**,不得自 merge。
- merge 时:把实质进展记入 `changelog.md`(带证据),并保留署名 trailer。
