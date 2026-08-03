# Node/Bun runnable corpus 100% campaign design (W43)

Date: 2026-08-02

Tracking issue: #80

Target branch: `rewrite_bun_in_mcpp`

Campaign branch: `agent/corpus-coverage-w43`
Approved direction: evidence-first five-hour sprint toward an unchanged final
100% runnable-corpus target

## 1. Goal and acceptance boundaries

This campaign has two explicit acceptance layers. They must never be collapsed
into one claim.

### 1.1 Final campaign goal

The final goal is 100% of the runnable, locally provisionable native Node and
Bun corpora on one frozen mbun binary:

| Corpus | Raw denominator | Currently excluded classification | Current runnable floor |
| --- | ---: | --- | ---: |
| Node `test/parallel` | 4,433 | 535 upstream self-skips | 3,898 / 3,898 |
| Bun `test/**` | 1,902 | 73 all-skipped, 6 no-tests, 19 blocked-external | 1,804 / 1,804 |
| Combined | 6,335 | 633 classified exclusions | 5,702 / 5,702 |

The raw `x/4433`, `x/1902`, and `x/6335` figures remain public beside the
runnable figures. Exclusions are not passes. The 3,898 and 1,804 denominators
are floors derived from the current classifications, not permission for future
runs to skip more files. A self-skip or all-skipped result that exists because
mbun lacks a capability remains actionable; only a pinned-upstream platform,
environment, or service exclusion survives the final audit. When implementing
a capability makes a file runnable, the runnable denominator grows and that
file must pass. A Bun `ahead-of-reference` result is not silently counted as
green: it is inspected and either converted to the current upstream contract
or retained as a named, evidence-backed category.

The last complete Linux measurement was made on the PR #36 merge tree rather
than current target commit `163cb6d`:

- Node: 3,136 pass, 662 fail, 535 skipped, 98 timeout, 2 oom-kill.
- Bun: 1,042 green, 701 test-failure, 73 all-skipped, 45 timeout,
  19 blocked-external, 4 crash, 4 load-error, 4 oom-kill, 6 no-tests,
  4 ahead-of-reference.

Those numbers imply a provisional runnable gap of 1,524 files. They are only a
planning input. The first W43 checkpoint replaces them with a full run on the
current target and one frozen binary.

### 1.2 First five-hour sprint goal

The five-hour sprint is an implementation checkpoint, not a redefinition of
final success. Its acceptance target is:

- fresh full Node and Bun baselines on current target commit `163cb6d`;
- net +30 to +45 real pass/fully-green files;
- Node contribution +22 to +33 and Bun contribution +8 to +12;
- every claimed new green file repeated serially on the candidate binary;
- zero previously green file regressions;
- no increase in timeout or oom-kill counts;
- refreshed full summaries, README metrics, committed data record, changelog,
  and batched PR milestone comments;
- Linux GCC 16.1.0 and LLVM 22.1.8 CI terminal states reported without
  inferring pending lanes.

Failure to reach +30 is reported as a missed sprint target with the measured
cause. It is not rewritten as success. Exceeding +45 does not remove the final
5,702/5,702 gate.

## 2. Why five hours cannot mean immediate 100%

The provisional runnable gap is 1,524 files. Historical long-tail measurements
show that a mechanism normally converts one to three files, with about ten
minutes of source reading, implementation, build, and focused measurement per
mechanism. Measured lane averages are approximately 3.4 Node files/hour and
1.7 Bun files/hour before coordinator integration overhead.

With this session's maximum of three simultaneous worker agents, a realistic
five-hour integrated result is +30 to +45 files. At a sustained net rate of
6–9 files/hour, the current provisional gap implies roughly 170–255 coordinator
wall-hours, or 34–51 five-hour sprints. The rate is recalculated after every
fresh full checkpoint from the trailing three waves; it is not presented as a
fixed delivery promise because the remaining tail can become harder or expose
a high-density shared cause.

Raw 6,335/6,335 is not an alternative completion claim: it would require
counting self-skips, no-tests, or unavailable external services as green, which
would weaken the repository's measurement contract.

## 3. Architecture and ownership boundaries

### 3.1 Coordinator

The coordinator owns all shared and authoritative operations:

- freezes the base commit and binary identity;
- runs release builds through `tools/integration/build_lock.sh` or
  `build_or_die.sh`;
- runs full Node/Bun corpus measurements exactly once per full checkpoint;
- generates disjoint worklists from the current result set;
- rejects source-touch overlap before dispatch;
- reviews and integrates lane commits;
- serially re-runs every claimed green file;
- runs impact and cross-corpus regression gates;
- updates README, `compat/data/mbun-corpus-runs.json`, `changelog.md`, and the
  W43 progress record from the same summaries;
- pushes milestone commits and publishes batched PR comments.

No worker may publish a whole-corpus number or update the shared metrics.

### 3.2 Worker lane

One logical lane has exactly one owner, one disjoint worklist, one predicted
source touch-set, and one quantitative target. A lane may contain multiple
test files only when source inspection supports a shared cause.

Each worker receives:

- exact base commit and isolated worktree path;
- exact corpus file list and before result rows;
- upstream Node/Bun source locations or a requirement to locate and cite them;
- allowed source area and forbidden overlapping hotspots;
- minimum/target green delta;
- focused reproduce command and timeout;
- required return payload: upstream mechanism, root cause, changed paths,
  before/after counts, serial evidence, and remaining failures.

Workers do not run full corpus suites, modify `compat/`, edit shared metrics,
or publish PR comments.

### 3.3 Integration artifacts

| Artifact | Owner | Responsibility |
| --- | --- | --- |
| `target/integration/w43-*-baseline/` | coordinator | immutable full baseline outputs |
| `target/integration/w43-*-candidate/` | coordinator | composed candidate outputs |
| lane-local `target/integration/w43-a1-before/` and corresponding lane directories | worker | focused red/green evidence |
| `.agents/docs/20260802-corpus-coverage-w43.md` | coordinator | durable progress and handoff ledger |
| `compat/data/mbun-corpus-runs.json` | coordinator | committed full-run provenance and counts |
| `README.md` | coordinator | concise current full-corpus metrics only |
| `changelog.md` | coordinator | substantive source progress and evidence |
| GitHub issue #80 | coordinator | campaign-level decisions and blockers |
| W43 Draft PR | coordinator | source changes, review, CI, milestone comments |

Target outputs are local evidence and are not staged. Committed metrics contain
repo-relative commands, commit identity, binary identity, run scope, resource
profile, counts, and timestamps; they contain no machine-specific path.

## 4. Source-driven three-stage development

Every lane uses the same three-stage contract. A lane that skips a stage is not
eligible for integration.

### Stage 1: Red / upstream comparison

1. Run the exact worklist through the bounded corpus runner and save the before
   result.
2. Re-run the deciding file serially to separate load noise from a stable
   failure.
3. Read the pinned upstream implementation under `compat/node/lib/`,
   `compat/node/src/`, `compat/bun/src/`, or the relevant upstream test helper.
4. Record the required mechanism and map it to the mbun implementation site.
5. Demonstrate that the failure is a missing or divergent mbun behavior rather
   than missing dependency provisioning, an upstream skip, or a stale binary.

Log-text clustering is only a worklist hint. It is never accepted as root-cause
evidence by itself.

### Stage 2: Green / semantic translation

1. Implement the smallest complete mechanism that matches the upstream
   contract.
2. Keep the common runtime kernel generic and Node/Bun dialect layers thin.
3. If the corpora intentionally require different behavior, use an explicit
   compatibility dispatch point; do not record the conflict as unreachable.
4. Add or extend a focused C++/JSC regression test when the upstream corpus
   alone cannot isolate the mechanism.
5. Rebuild through the serialized build lock and run the exact worklist with
   the same parameters as Stage 1.

No code change may weaken, edit, or replace an assertion under `compat/`.

### Stage 3: Refactor / proof

1. Remove duplication and keep runtime slices within existing structural
   limits while the focused tests remain green.
2. Run each newly green file with `--jobs 1` on the frozen candidate binary.
3. Run the diff-derived impact gate and any shared Node/Bun counterpart files.
4. Compare before/after with `tools/integration/corpus_diff.py`; bucket totals
   alone are insufficient because assertion counts can move inside a bucket.
5. Commit one independent defect with explicit paths, issue reference,
   builder `Signed-off-by`, and Codex co-author trailer.

## 5. Parallel execution model

The user requested 5–10 agents per round. This runtime exposes four total
concurrency slots, including the coordinator, so at most three worker agents
can run simultaneously. W43 therefore defines five logical lanes per wave and
executes them in two rolling batches:

```text
coordinator: baseline / review / build / integration / full verification
batch 1:     lane 1 + lane 2 + lane 3
batch 2:     lane 4 + lane 5 + next free validation slot
```

This is reported publicly as five lanes with three-worker physical concurrency,
never as five simultaneous agents. Every worker uses a separate worktree.
`build_lock.sh` keeps one active build, and worker corpus runners use at most
`--jobs 3`. Resource pressure may reduce worker jobs but cannot increase them
without a coordinator check of memory, swap, tasks, and disk.

### 5.1 Conflict exclusions

The following pairs cannot be separate simultaneous owners:

- crypto and webcrypto: shared key and OpenSSL bridges;
- Node HTTP and Bun HTTP: shared parser/transport/response machinery;
- async_hooks, test-runner, and worker scheduling: shared bootstrap/event pump;
- two N-API lanes: shared `runtime/napi*.inc` slices;
- two changes to the same JS builtin payload or runtime include slice.

Before dispatch, the coordinator compares predicted touch-sets. If two lanes
overlap, the lower-value lane is replaced with the next disjoint lane rather
than relying on a later conflict resolution.

## 6. Five-hour execution design

### 6.1 Time and gates

| Window | Work | Quantitative gate | PR synchronization |
| --- | --- | --- | --- |
| 0:00–0:45 | build current target; full Node+Bun baseline on one binary | exact current pass/green, failure buckets, runnable gap | open Draft PR with issue/design/baseline; comment checkpoint 0 |
| 0:45–2:05 | Wave A: five lanes in two batches | +12–20 net green | no routine probe comments |
| 2:05–2:30 | integrate Wave A; serial and impact gates | zero regression; timeout/OOM non-increase | push checkpoint 1 and one batched comment |
| 2:30–3:50 | Wave B: five lanes in two batches | +12–22 net green | no routine probe comments |
| 3:50–4:20 | integrate Wave B; serial and impact gates | cumulative +30–45 target | push checkpoint 2 and one batched comment |
| 4:20–5:00 | full Node+Bun candidate measurement and metric sync | full summaries, final delta, honest misses | final sprint comment with CI state and next route |

The two full corpora took approximately 38 minutes sequentially in the latest
measurement. The design reserves 40 minutes for the final pair. If a run is
slower, final full evidence takes priority over starting another lane.

### 6.2 Wave A candidate lanes

The baseline planner must confirm these files remain red before dispatch.

| Lane | Scope | Target delta | Predicted touch area |
| --- | --- | ---: | --- |
| A1 | Node zlib/Buffer validation leaves | +2 to +5 | zlib/buffer validation paths |
| A2 | Node assert near-green leaves | +2 to +4 | Node assert builtin only |
| A3 | Node permission or V8 validation leaves | +2 to +4 | permission gate or V8 builtin, one selected after touch-set check |
| A4 | Bun N-API near-green files | +3 to +5 | N-API runtime slices |
| A5 | Bun test-runner near-green files | +3 to +5 | test-runner payload and isolated runner bridges |

Wave A's integration target is +12 to +20. If a candidate is already green or
shares a source hotspot, it is replaced by a current near-green file from Bun
`regression/issue` or `third_party`, with its own fresh red proof and target.

### 6.3 Wave B candidate lanes

Wave B worklists are generated after checkpoint 1 and may change based on the
new failure frontier. The initial disjoint candidates are:

| Lane | Scope | Target delta | Predicted touch area |
| --- | --- | ---: | --- |
| B1 | Node REPL leaves excluding struck inspector work | +2 to +5 | REPL builtin |
| B2 | combined Node crypto/webcrypto owner | +2 to +5 | one shared crypto owner |
| B3 | Bun third-party leaf failures | +2 to +4 | package-specific public API gap |
| B4 | Bun CLI/run leaf failures | +2 to +4 | CLI/run dispatch |
| B5 | Node fs one-off mechanisms | +2 to +4 | fs builtin/runtime only |

Wave B's integration target is +12 to +22. No B lane starts from a stale W42
result: every target must be present in the W43 baseline or checkpoint-1
candidate output.

## 7. Measurement and test interfaces

### 7.1 Full baseline and final runs

Node:

```bash
W43_BIN_REL=$(find target -name mbun -type f -printf '%T@ %p\n' \
  | sort -rn | head -1 | cut -d' ' -f2)
W43_BIN="$PWD/$W43_BIN_REL"
sha256sum "$W43_BIN_REL"

python3 tools/integration/node_corpus_runner.py \
  --bin "$W43_BIN" \
  --out target/integration/w43-node-baseline \
  --jobs 4 \
  --timeout 15
```

Bun:

```bash
python3 tools/integration/bun_corpus_runner.py \
  --bin "$W43_BIN" \
  --cwd compat/bun \
  --discover compat/bun/test \
  --sample-per-group 100000 \
  --out target/integration/w43-bun-baseline \
  --jobs 4 \
  --timeout 30
```

The coordinator resolves `W43_BIN` once from the newest mtime after its build
and records `W43_BIN_REL` plus the checksum. The same path is passed to both
runners. A run made against a
different binary cannot be compared as the same checkpoint.

### 7.2 Focused lane runs

Node lanes use `--files` or a narrowly justified `--filter` against the exact
worklist. Bun lanes use the exact file-list interface supported by the current
runner; the plan records the generated list path and command after checking the
runner's live `--help`. Worker jobs are at most three; deciding re-runs use one.

Every spawn, socket, server, worker, install, and hang-prone test remains behind
the runners or `tools/integration/safe-test.sh`. Bare execution is forbidden.

### 7.3 Regression decision

A change is integrable only when all of the following are true:

- its stable red moved to green for the intended upstream reason;
- every new green repeats serially;
- no file in its focused before set moves pass/green to a worse category;
- assertion counts do not regress in shared Bun files;
- the diff-derived impact set is green or unchanged;
- a relevant member/JSC test passes;
- the coordinator understands the source diff and issue mapping.

## 8. Error handling and replanning

| Condition | Required action |
| --- | --- |
| stale binary or mismatched commit | discard the run; rebuild and repeat |
| missing Bun dependencies | stop before measurement; provision with frozen lockfiles |
| timeout or spawn storm | sandbox kills it; preserve log; reduce jobs, never run bare |
| OOM or swap pressure | stop new dispatch; lower jobs/concurrency; publish a resource-strategy milestone if schedule changes |
| parallel-only regression | reproduce serially before attributing it to source |
| worker misses target | report actual delta and root cause; do not widen scope without coordinator review |
| overlapping source edits | serialize or replace the lower-value lane before implementation |
| upstream Node/Bun conflict | create explicit dialect-dispatch issue/design; do not mark unreachable |
| external service/toolchain requirement | classify `blocked-external` with evidence; do not count green |
| CI pending/cancelled/superseded | state that exact status; only final-head terminal jobs support a CI claim |
| five-hour clock threatens final verification | stop dispatch and spend remaining time on composed full evidence |

## 9. Git, issue, and PR lifecycle

1. Campaign tracker #80 owns the overall acceptance definition and sprint
   decisions.
2. Each independently fixed defect links an existing issue or gets a new issue
   with environment, reproduction, actual error, upstream comparison, and
   suspected source owner.
3. The coordinator branch starts at `origin/rewrite_bun_in_mcpp@163cb6d` and
   preserves history through ordinary commits and pushes; no amend, rebase, or
   force-push.
4. The Draft PR targets `rewrite_bun_in_mcpp`, not `main`, and links #80.
5. Commits use conventional subjects, a builder primary author and
   `Signed-off-by`, plus `Co-authored-by: Codex (GPT-5) <>`.
6. Explicit paths are staged. `git add -A`, `git commit -a`, and corpus gitlink
   changes are forbidden.
7. W43 does not modify protected hagent, skill, CI, license, or governance
   surfaces. If implementation discovers that such a change is required, it is
   split out and awaits maintainer approval.

## 10. PR milestone comment contract

Only meaningful milestones produce comments:

1. **Checkpoint 0 — fresh baseline:** base/head, frozen binary identity, full
   Node/Bun counts, runnable gap, worklists, physical concurrency disclosure.
2. **Checkpoint 1 — Wave A integrated:** source commits/issues, lane targets vs
   actuals, serial proof, zero-regression result, blockers, Wave B changes.
3. **Checkpoint 2 — Wave B integrated:** cumulative target vs actual, build and
   impact gates, any resource or strategy change.
4. **Final sprint checkpoint:** fresh full before→after, raw and runnable rates,
   timeout/crash/OOM movement, CI terminal/pending states, missed requirements,
   recalculated remaining gap and ETA.

Each comment states whether its data is full or focused. Comments contain only
repo-relative paths and sanitized error excerpts; no user name, host name,
local absolute path, token, environment value, private URL, or machine ID.

## 11. Compatibility and risk

- `compat/` is a read-only oracle. Portability adapters live outside it and may
  not weaken assertion semantics.
- Node semantics govern `node:*` APIs unless the pinned Bun corpus specifies a
  genuine, intentional dialect difference. Such differences use an explicit
  compatibility dispatch point.
- Shared runtime changes carry a dual-corpus assertion-count gate, not merely a
  green-file gate.
- Linux x86_64 is the corpus measurement platform. macOS CI is reported
  separately and never used to inflate Linux coverage.
- Current target CI run status is not inherited by the campaign; the Draft PR's
  final head must obtain its own terminal results.
- The biggest schedule risks are long-tail heterogeneity, stale worklists,
  shared JSC hotspots, full-run duration, resource contention, and external
  dependencies. The ownership and stop rules above bound each risk without
  redefining success.

## 12. Review checklist

- Final target is at least Node 3,898/3,898 and Bun 1,804/1,804; capability
  enablement may raise the audited runnable denominators but never lower them.
- Raw denominators and excluded categories stay visible.
- Five-hour success is +30 to +45, not an immediate 100% claim.
- Latest-target full baseline precedes lane implementation.
- Five logical lanes run with at most three physical workers.
- Every lane uses Red/upstream comparison, Green/translation, Refactor/proof.
- Coordinator alone owns builds, full runs, integration, metrics, and comments.
- Every source fix is issue-first and commit-scoped.
- Final verification takes precedence over extra dispatch.
- No placeholder, unassigned decision, or hidden external dependency remains in
  this design.
