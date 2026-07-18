# Agents

[中文](zh/agents.md)

The tool-neutral operating entry for any coding agent working in this repository.
Authority is the [charter](charter.md); participation basics are in
[contributing.md](contributing.md). This page is your checklist.

## Read the right skill first

Operating procedures live in [`.agents/skills/`](../.agents/skills/). Read the
one that matches your task **before** acting — don't re-derive it:

| When | Skill |
| --- | --- |
| Writing or reviewing C++ modules (naming, `.cppm`, module layout) | [`mcpp-style-ref`](../.agents/skills/mcpp-style-ref/SKILL.md) |
| Developing any feature or fixing any bug | [`tdd-workflow`](../.agents/skills/tdd-workflow/SKILL.md) |
| A runtime test fails / crashes / hangs | [`mbun-runtime-debugging`](../.agents/skills/mbun-runtime-debugging/SKILL.md) |

## Hard rules

- **Sandbox everything that can spawn or hang.** Run through
  `tools/integration/safe-test.sh` / `bounded_run.py` — never bare. (A stray
  fork storm can freeze the machine.)
- **`compat/` is read-only.** Adapt for portability (paths, runner bridging), but
  never weaken assertion semantics.
- **Evidence before "done."** Build passing ≠ feature done. Provide suite
  before→after counts + a reproduce command (see
  [contributing.md](contributing.md#evidence)).
- **Conventional commits**, one dev-item per commit; prefer separate red/green
  commits.
- **Target branch.** Open PRs against **`rewrite_bun_in_mcpp`**, never `main`.

## Sign every commit to a builder

You are always a **co-author**, never the primary author. Use the format in
[contributing.md](contributing.md#commit-convention): builder `Signed-off-by:` +
your `Co-authored-by:` line (agent name + model; official no-reply email, or `<>`
if none). The builder you sign to must be able to **understand the diff**, so
keep it reviewable.

## Don't self-merge protected surfaces

`hagent/**`, `.agents/skills/**`, the root pointers, `.github/workflows/**`,
`CODEOWNERS`, `LICENSE` (charter §5) require **maintainer sign-off**. Open the
PR; do not merge it yourself.

## Coordinate through the changelog

`changelog.md` is the shared work log and hand-off protocol:

- Record substantive progress with evidence (suite `x/y → x'/y'`, zero-regression
  claims backed by before/after).
- Leave enough that the next session/agent can pick up without re-discovery.
- Check it before starting to avoid duplicating or colliding with in-flight work.
