# Labels

[中文](zh/labels.md)

A small, bilingual label set (`中文 | English`) that both a builder and the
agent they run can read and apply. Each label's description says **when to apply
it**. 10 labels in 4 groups.

## Type — what the change is (aligns with conventional commits)

| Label | Color | Apply when |
| --- | --- | --- |
| `特性 \| feature` | `#0e8a16` | New feature |
| `缺陷 \| bug` | `#d73a4a` | Bug fix |
| `测试 \| test` | `#fbca04` | Tests / corpus only |
| `文档 \| docs` | `#0075ca` | Docs only |

## Provenance — who authored it (hagent's measurement axis)

| Label | Color | Apply when |
| --- | --- | --- |
| `AI 主导 \| agent-led` | `#5319e7` | Mostly agent-authored |
| `构建者主导 \| builder-led` | `#1d76db` | Mostly builder-authored |

## Flow — collaboration signal on the issue / PR

| Label | Color | Apply when |
| --- | --- | --- |
| `待认领 \| available` | `#c2e0c6` | Issue is open to claim (by a builder or agent) |
| `待维护者审 \| needs-maintainer` | `#b60205` | Needs **maintainer sign-off** (governance / protected surface) |
| `需修改 \| changes-requested` | `#e99695` | Rework needed after review |

## Guard

| Label | Color | Apply when |
| --- | --- | --- |
| `受保护面 \| protected` | `#d93f0b` | Touches a protected surface — `hagent/` · `.agents/` · `.github/` · `LICENSE` ([charter §5](charter.md#5-protected-surfaces-maintainer-sign-off-required)) |

## Who applies them

- **Agents and builders self-apply** `Type` and `Provenance` when opening a PR,
  and `available` when filing a claimable issue.
- `受保护面 \| protected` and `待维护者审 \| needs-maintainer` are the signals that
  move a PR off the "agent may self-merge" path onto the **maintainer sign-off**
  path — see [charter §4](charter.md#4-permission-tiers) and
  [§5](charter.md#5-protected-surfaces-maintainer-sign-off-required).

> Understanding the diff is **not** a label — it is the default: the builder a
> contribution is attributed to must understand it ([charter §3](charter.md#3-attribution-every-action-belongs-to-a-builder)).
