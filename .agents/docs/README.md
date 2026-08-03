# .agents/docs —— 开发 / 设计方案

存放**新功能的开发 / 设计方案**(design proposals)。

新功能开发前,经 issue 充分讨论后,在此新建 `<topic>.md`,至少包含:

- **目标** —— 要解决什么、验收标准
- **模块边界** —— 落在哪个 workspace 成员 / 新模块
- **接口设计** —— 对外 API / 行为,与 bun/node 的对照
- **测试计划** —— 上游语料锚点或成员单测
- **兼容性与风险** —— 影响面、回归面、替代方案

流程见 [`.agents/skills/dev-process`](../skills/dev-process/SKILL.md)。
