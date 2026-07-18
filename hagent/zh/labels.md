# 标签

[English](../labels.md) · 以英文本 [`../labels.md`](../labels.md) 为准。

一套精简的双语标签(`中文 | English`),构建者与其运行的 agent 都能识别并打。每个
标签的描述说明**何时打**。共 **10 个**,分 4 组。

## 类型 Type —— 改动是什么(对齐 conventional commits)

| 标签 | 颜色 | 何时打 |
| --- | --- | --- |
| `特性 \| feature` | `#0e8a16` | 新功能 |
| `缺陷 \| bug` | `#d73a4a` | 修复缺陷 |
| `测试 \| test` | `#fbca04` | 仅测试 / 语料 |
| `文档 \| docs` | `#0075ca` | 仅文档 |

## 来源 Provenance —— 谁产出的(hagent 的度量轴)

| 标签 | 颜色 | 何时打 |
| --- | --- | --- |
| `AI 主导 \| agent-led` | `#5319e7` | 主要由 agent 完成 |
| `构建者主导 \| builder-led` | `#1d76db` | 主要由构建者完成 |

## 流转 Flow —— issue / PR 的协作信号

| 标签 | 颜色 | 何时打 |
| --- | --- | --- |
| `待认领 \| available` | `#c2e0c6` | issue 开放认领(构建者或 agent) |
| `待维护者审 \| needs-maintainer` | `#b60205` | 需**维护者签字**(治理 / 受保护面) |
| `需修改 \| changes-requested` | `#e99695` | 审查后需返工 |

## 守卫 Guard

| 标签 | 颜色 | 何时打 |
| --- | --- | --- |
| `受保护面 \| protected` | `#d93f0b` | 触及受保护面 —— `hagent/`·`.agents/`·`.github/`·`LICENSE`([宪法 §5](charter.md#5-受保护面须维护者签字)) |

## 谁来打

- **agent 与构建者自助打** `类型` 与 `来源`(开 PR 时),以及 `待认领`(建可认领 issue 时)。
- `受保护面 \| protected` 与 `待维护者审 \| needs-maintainer` 是把 PR 从「agent 可自
  merge」引到**维护者签字**路径的信号——见 [宪法 §4](charter.md#4-权限层级) 与
  [§5](charter.md#5-受保护面须维护者签字)。

> 读懂 diff **不是**标签,而是默认:贡献归属到的构建者必须读懂它([宪法 §3](charter.md#3-归属每个操作都归属到构建者))。
