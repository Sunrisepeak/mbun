# Contributing

[中文](zh/contributing.md)

For builders and the agents they bring. Read the [charter](charter.md) for the
model behind these rules; this page is the practical how-to.
("Builder" = the human who runs the agent and is accountable for the result.)

## Setup

Build from source and run the examples per the project
[README](../README.md#quick-start). Agents should also read [agents.md](agents.md).

## What a good contribution looks like

- **One dev-item per PR.** Keep scope tight; unrelated changes go in separate PRs.
- **Target branch.** Open PRs against **`rewrite_bun_in_mcpp`** — **not** `main`.
- **TDD.** Follow [`tdd-workflow`](../.agents/skills/tdd-workflow/SKILL.md):
  red → green → refactor, with the upstream test corpus as the acceptance signal.
- **Conventional commits.** `type(scope): subject` with
  `feat` / `fix` / `test` / `refactor` / `docs` / `chore` / `research`.
- **Respect the bottom lines** (charter §7): `compat/` is read-only; don't weaken
  assertions; run spawn/hang-prone tests only through the sandbox
  (`tools/integration/safe-test.sh`).

## Commit convention

Every commit — including work an agent performed — has a **builder as primary
author and signer**, and the **agent as co-author**:

```
feat(html_rewriter): support element.setAttribute on void elements

- #123

Void elements dropped attribute writes because the serializer skipped
their attribute list. Route them through the shared attribute path and
add the upstream fixture as a member test.

Signed-off-by: Sunrisepeak <speakshen@163.com>
Co-authored-by: Claude (Opus 4.8) <noreply@anthropic.com>
```

- **Title** — the conventional-commit subject.
- **Related issues** — list them as `- #<n>`, one per line; GitHub auto-links and
  expands them (use `Closes #<n>` to close an issue on merge).
- **Description** — what changed and why.
- **`Signed-off-by:`** — the builder. `git commit --author` / config keeps
  `Author` = the builder's email too.
- **`Co-authored-by:`** — the agent and its model; GitHub renders it. Use the
  agent's official no-reply email if it has one, otherwise leave it empty (`<>`).

Common agents (replace `<model>` with the actual model):

| Tool | `Co-authored-by:` line |
| --- | --- |
| Claude / Claude Code | `Co-authored-by: Claude (<model>) <noreply@anthropic.com>` |
| OpenAI Codex | `Co-authored-by: Codex (<model>) <>` |
| Cursor | `Co-authored-by: Cursor (<model>) <>` |
| OpenCode | `Co-authored-by: opencode (<model>) <>` |
| GitHub Copilot | `Co-authored-by: GitHub Copilot (<model>) <>` |

The builder the commit is signed to **must understand the diff** (charter §3) —
this is the default, not a special step.

## Evidence

Any PR claiming a test or behavior change **must** show reproducible evidence
(enforced socially via the [PR template](../.github/PULL_REQUEST_TEMPLATE.md), not
yet by CI):

- **Suite:** `<file/group>` — before `x/y` → after `x'/y'`
- **Reproduce:** the runner command, or a link to `compat/data`

Build passing is **not** evidence of a behavior change.

## Getting permission

- **Triager** — self-service once your account has **≥ 3 merged PRs** (agents
  included): triage, label, confirm bugs, verify issues/PRs, run CI.
- **Committer** (write) — relevant module background + significant
  contributions, with **maintainer approval**.
- Higher tiers (module / project maintainer) are still being defined.

See charter [§4](charter.md#4-permission-tiers).

## Review & merge

- Agent review is the default path; a green PR outside the protected surfaces may
  be merged by an agent reviewer.
- A PR that touches a **protected surface** (charter §5) or is high-risk requires
  **maintainer sign-off** and cannot be self-merged.
- On merge: record substantive progress in `changelog.md` with evidence, and keep
  the attribution trailers.
