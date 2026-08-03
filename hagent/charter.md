# hagent charter

[中文](zh/charter.md)

The constitution for how builders and agents collaborate in this repository.
It is deliberately short. When a rule here conflicts with any other document,
this charter wins.

("Builder" = the human who creates and judges — see §2.)

## 1. Roles

| Role | Owns | May not |
| --- | --- | --- |
| **Builder** | Architecture, decisions, taste, norms; understanding and reviewing what is merged under their name (§3). | — |
| **Maintainer** | The protected surfaces (§5); granting write permission (§4). A builder with governance authority. | — |
| **Agent contributor** | Implementation, tests, changelog, opening PRs — under a builder's identity (§3). | Self-merge protected surfaces (§5); grant its own write permission. |
| **Agent reviewer** | Reviewing and approving ordinary PRs; merging when green and outside protected surfaces. | Approve changes to protected surfaces without a maintainer. |

## 2. Division of labor: execution vs. creation

Work is split by its **nature**, not its volume.

- **Agents execute**: implementation, tests, PR review, merging ordinary changes.
- **Builders create and judge**: architecture, decisions, taste, norms, and a
  minority of special detail design/optimization. The builder is the human who
  participates in building and creating — not merely a gatekeeper.
- Agents carrying 90%+ of the throughput is a *consequence* of execution being
  most of the volume — not the governing rule.

## 3. Attribution: every action belongs to a builder

Even when an agent does the work:

- The **primary author and signer is a builder** (commit `Author` = the
  builder's email; `Signed-off-by:` = the builder).
- The **agent is a co-author**, recorded with its model in a `Co-authored-by:`
  line (see [contributing.md](contributing.md#commit-convention) for per-tool lines).
- Because a contribution is attributed to a builder, that builder **must
  understand the diff** they submit — this is the default, not a special case.

The exact commit format lives in [contributing.md](contributing.md#commit-convention).
This keeps a single, builder line of accountability while making agent
contribution measurable.

## 4. Permission tiers

Elevated trust is a ladder; agents earn the same tiers by the same rules. Tiers
above committer are still being defined.

| Role | Grants | How to obtain |
| --- | --- | --- |
| **Triager** | Triage: apply labels, confirm/reproduce issues, verify issues and PRs, request changes, and run CI on PRs. | **Self-service** after **≥ 3 merged PRs** (agents included). |
| **Committer** | Write (push / merge). | Relevant module background **and** significant contributions; **maintainer approval**. |
| **Module maintainer** | Review and decisions for a module. | — |
| **Project maintainer** | Overall governance and the protected surfaces (§5). | — |

This is the lean, first-cut of the full identity & permission model (roadmap
module ⑤).

## 5. Protected surfaces (maintainer sign-off required)

Changing any of these is a *constitutional* change: an agent may **not**
self-merge it, and a maintainer must sign off — in **both** directions (if an
agent proposes it, a maintainer approves; if a builder proposes it, the change
is still recorded and reviewed).

- `hagent/**` — this system itself
- `.agents/skills/**` — agent operating procedures (SOP)
- `CONTRIBUTING.md` / `AGENTS.md` / `CLAUDE.md` / `GOVERNANCE.md` — root pointers
- `.github/workflows/**` — CI
- `CODEOWNERS` (future) and `LICENSE`

> This charter states the list. Its *enforcement* (CODEOWNERS, branch
> protection) is roadmap module ③.

## 6. Contribution lifecycle

```
claim ──▶ branch ──▶ TDD (red→green) ──▶ gather evidence ──▶ open PR (template)
                                                                │
                                                                ▼
                                    agent review ──▶ gates (evidence / style / regression)
                                                                │
                          ┌─────────────────────────────────────┴──────────────────┐
                          ▼                                                          ▼
                 outside protected surface & green                    protected surface OR high-risk
                          │                                                          │
                          ▼                                                          ▼
                 agent may self-merge                                  maintainer sign-off required
                          │                                                          │
                          └────────────────────────────┬─────────────────────────────┘
                                                        ▼
                              merge ──▶ record in changelog.md + attribution trailers
```

## 7. Acceptance bottom lines

From mbun's [`tdd-workflow`](../.agents/skills/tdd-workflow/SKILL.md):

- The upstream corpora under `compat/` are **read-only inputs** — portability
  adaptation is allowed, but assertion semantics must not be weakened.
- **No "done" without evidence** — suite before→after counts + a reproduce
  command (see [contributing.md](contributing.md#evidence)).
- Tests that may spawn/hang run only through the bounded sandbox
  (`tools/integration/safe-test.sh` / `bounded_run.py`).

## 8. Roadmap

hagent grows one module per iteration; each gets its own spec → plan →
implementation and adds a section/file here.

| # | Module | Status |
| --- | --- | --- |
| ① | Charter (this file) | ✅ in place |
| ② | Entry docs (contributing, agents) | ✅ in place |
| ③ | Protected-surface enforcement (CODEOWNERS, branch protection) | 🗺️ planned |
| ④ | [Label system](labels.md) (bilingual taxonomy) | ✅ in place |
| ⑤ | Identity & permission model (bot accounts, tiers beyond §4) | 🗺️ planned |
| ⑥ | Automation workflow (CI gates, evidence checks, auto-merge) | 🗺️ planned |
