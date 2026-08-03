# hagent — AI Agent open-source collaboration spec

[中文](zh/README.md)

> **hagent** = **h**uman + **agent**. A lean, reusable collaboration spec for
> open source in the AI era. Born inside [mbun](../README.md) as its goal #3
> experiment ("validating and exploring AI-agent-driven development &
> collaboration"), and dogfooded here first. It is designed to be liftable into
> its own repository later.

> [!NOTE]
> **Status: experimental.** Modules ① charter, ② entry docs, and ④ labels are in
> place. Modules ③, ⑤, ⑥ (protected-surface enforcement, permission automation,
> CI gates) are named in [charter.md](charter.md) as a roadmap and land in later
> iterations.

## Core thesis

**Division of labor by the *nature* of the work, not by volume.**

- **Agent = execution engine** — implementation, tests, PR review, and merging
  ordinary changes. This is 90%+ of the throughput, so it naturally lands on
  agents.
- **Builder = the human who creates and judges** — **architecture, decisions,
  taste, and norms**, plus a minority of special detail design and optimization.
  The builder *participates in building and creating*; they are not merely a
  gatekeeper.
- **90%+ is a result, not the definition.** Execution is most of the volume, so
  most of the volume is the agent's.

## Two more principles

- **Every action is attributed to a builder.** Even work an agent performs is
  committed under a builder's identity (the builder is the primary author and
  signer); the agent is recorded as a **co-author** with its model, and the
  builder must understand the diff. See the commit convention in
  [contributing.md](contributing.md).
- **Evidence before "done."** No claim of "passing / green / zero-regression" is
  accepted without reproducible evidence (suite before→after counts + how to
  reproduce). See [contributing.md](contributing.md) and the
  [PR template](../.github/PULL_REQUEST_TEMPLATE.md).

## What's here

| File | For | What it is |
| --- | --- | --- |
| [charter.md](charter.md) | everyone | The constitution: roles, the execution-vs-creation split, attribution, permission tiers, protected surfaces, the contribution lifecycle, and the roadmap. |
| [contributing.md](contributing.md) | outward (builders + their agents) | How to participate: setup, what a good contribution looks like, the commit & attribution convention, evidence, and the sandbox bottom line. |
| [agents.md](agents.md) | inward (any coding agent) | The tool-neutral operating entry: which skill to read when, the hard rules, coordination via the changelog, and how to sign. |
| [labels.md](labels.md) | everyone | The bilingual label set (`中文 \| English`) and when to apply each. |

Root pointers `CONTRIBUTING.md`, `AGENTS.md`, `CLAUDE.md`, `GOVERNANCE.md`
forward here so tooling and GitHub discover them automatically.

Agent operating procedures live in mbun's [`.agents/skills/`](../.agents/skills/)
(`mcpp-style-ref`, `tdd-workflow`, `mbun-runtime-debugging`); `agents.md`
navigates them.
