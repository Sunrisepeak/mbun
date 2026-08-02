# W43 Node/Bun Runnable Corpus Sprint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a fresh same-binary Node/Bun full baseline, land two bounded five-lane source-driven waves, and prove a net +30 to +45 pass/fully-green files without hiding regressions or changing the final 100% runnable-corpus goal.

**Architecture:** The coordinator owns the frozen binary, full measurements, worklist disjointness, integration, metrics, and GitHub updates. Five logical lanes per wave run in isolated worktrees with at most three workers active; every lane follows Red/upstream comparison, Green/semantic translation, and Refactor/proof before its commit can be integrated.

**Tech Stack:** C++26 modules, JavaScriptCore C API, embedded JavaScript builtins, Python corpus runners, `mcpp 2026.7.31.1`, GCC 16.1.0, LLVM 22.1.8, Git worktrees, GitHub issue/PR workflow.

## Global Constraints

- Target `rewrite_bun_in_mcpp`; never target `main`.
- Freeze W43 at `163cb6d3fedb9d22cf069cb5f8e20fcb2bc76049`; target movement after the sprint starts is handled by a later ordinary merge, never rebase or force-push.
- Final acceptance is at least Node 3,898/3,898 and Bun 1,804/1,804 runnable files; capability enablement may increase but never decrease those denominators.
- The first five-hour checkpoint targets net +30 to +45: Node +22 to +33 and Bun +8 to +12.
- `compat/bun/**` and `compat/node/**` are read-only upstream inputs; assertion semantics are immutable.
- All corpus execution uses `node_corpus_runner.py`, `bun_corpus_runner.py`, `safe-test.sh`, or `bounded_run.py`; no bare spawn/hang-prone test.
- Full corpus measurement and release builds belong only to the coordinator.
- Worker runs use at most `--jobs 3`; deciding re-runs use `--jobs 1`; builds serialize through `build_lock.sh`.
- One coordinator plus three workers is the physical ceiling. A wave has five logical lanes in two rolling batches; public data states both numbers.
- Every independent source fix is issue-first, one substantive conventional commit, explicit staging, builder `Signed-off-by`, and `Co-authored-by: Codex (GPT-5) <>`.
- Never use `git add -A`, `git commit -a`, amend, rebase, force-push, or stage corpus gitlink changes.
- Full/focused scope labels, exact before→after counts, final-head CI status, and unresolved blockers are mandatory in PR comments.
- Do not publish user names, host names, absolute local paths, tokens, environment values, machine identifiers, or private URLs.

---

## File and responsibility map

- `.agents/docs/20260802-corpus-coverage-w43-design.md`: approved acceptance and architecture; do not rewrite during implementation except to correct a proven factual error.
- `.agents/docs/20260802-corpus-coverage-w43-plan.md`: this executable plan and checkbox record.
- `.agents/docs/20260802-corpus-coverage-w43.md`: create in Task 2; durable baseline, lane, integration, and final evidence ledger.
- `tools/integration/manifests/w43-a1-node-zlib-buffer.txt`: exact Wave A1 corpus inputs.
- `tools/integration/manifests/w43-a2-node-assert.txt`: exact Wave A2 corpus inputs.
- `tools/integration/manifests/w43-a3-node-permission.txt`: exact Wave A3 corpus inputs.
- `tools/integration/manifests/w43-a4-bun-napi.txt`: exact Wave A4 corpus inputs.
- `tools/integration/manifests/w43-a5-bun-test-runner.txt`: exact Wave A5 corpus inputs.
- `target/integration/w43-node-baseline/` and `w43-bun-baseline/`: authoritative local baseline; never stage.
- `target/integration/w43-*-before/` and `w43-*-after/`: worker-local focused evidence; never stage.
- `compat/data/mbun-corpus-runs.json`: update only from completed full baseline/final summaries on the recorded binary.
- `README.md`: publish concise full-corpus metrics only.
- `changelog.md`: record substantive source movement and exact evidence.
- `modules/jsc/src/builtins/*.cppm`, `modules/jsc/src/runtime/*.inc`, `modules/jsc/src/runtime/napi/*`: source areas selected only after Stage 1 proves the upstream mechanism.
- `modules/jsc/tests/*.cpp`: focused member regression tests for mechanisms that need an isolated native/JSC seam.

---

### Task 1: Freeze and verify the coordinator base

**Files:**
- Verify: `.agents/docs/20260802-corpus-coverage-w43-design.md`
- Verify: `.agents/docs/20260802-corpus-coverage-w43-plan.md`
- Verify: `compat/bun`, `compat/node`

**Interfaces:**
- Consumes: commit `08a4de9f7d84710cc7d22f7998accfad403bd626` on branch `agent/corpus-coverage-w43`.
- Produces: a clean coordinator worktree whose only diff from `163cb6d` is the design and plan commits.

- [x] **Step 1: Confirm branch, ancestry, and clean scope**

```bash
git status --short --branch
git merge-base --is-ancestor 163cb6d HEAD
git diff --name-status 163cb6d...HEAD
```

Expected: branch `agent/corpus-coverage-w43`; ancestry command exits 0; diff contains only the two `.agents/docs` files before baseline recording.

- [x] **Step 2: Prove corpus gitlinks and paths**

```bash
tools/integration/check_submodule_gitlinks.sh
test -e compat/node/test/parallel/test-assert-async.js
test -e compat/bun/test/js/bun/test/done-async.test.ts
git ls-files -s compat/bun compat/node
```

Expected: checker prints `clean`; both paths exist; both index entries remain mode `160000`.

- [x] **Step 3: Confirm Bun dependency provisioning**

```bash
test -d compat/bun/node_modules
test -d compat/bun/test/node_modules
```

Expected: both commands exit 0. If either fails, run the repository-documented frozen installs from `compat/README.md` before any Bun measurement and record that provisioning time separately from runtime implementation time.

### Task 2: Build one binary and take the fresh full baseline

**Files:**
- Create: `.agents/docs/20260802-corpus-coverage-w43.md`
- Modify: `compat/data/mbun-corpus-runs.json`
- Evidence only: `target/integration/w43-node-baseline/`
- Evidence only: `target/integration/w43-bun-baseline/`

**Interfaces:**
- Consumes: clean Task 1 tree at `08a4de9`.
- Produces: `W43_BIN`, its SHA-256, full `summary.json`/`results.tsv` for both corpora, exact current runnable gap, and a committed baseline record.

- [x] **Step 1: Build through the global lock and capture the binary**

```bash
W43_BIN=$(bash tools/integration/build_or_die.sh)
test -x "$W43_BIN"
W43_BIN_SHA=$(sha256sum "$W43_BIN" | awk '{print $1}')
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_BASE_BIN="$W43_COMMON_DIR/w43/baseline/mbun"
install -Dm755 "$W43_BIN" "$W43_BASE_BIN"
test "$(sha256sum "$W43_BASE_BIN" | awk '{print $1}')" = "$W43_BIN_SHA"
tools/integration/safe-test.sh 10 "$W43_BIN" --version
```

Expected: build exits 0; version reports mbun 2026.07.18.0, Bun 1.3.14 compatibility, and Node v26.3.0 compatibility. The shared frozen copy under the Git common directory has the same SHA-256 and is readable from every lane worktree. Record the coordinator-relative binary path and `W43_BIN_SHA` in the W43 ledger.

- [x] **Step 2: Run the full Node baseline**

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_BASE_BIN="$W43_COMMON_DIR/w43/baseline/mbun"
python3 tools/integration/node_corpus_runner.py \
  --bin "$W43_BASE_BIN" \
  --root "$PWD" \
  --out target/integration/w43-node-baseline \
  --jobs 4 \
  --timeout 15
```

Expected: `summary.json` exists and category sum equals 4,433.

- [x] **Step 3: Validate the Node denominator**

```bash
jq -e '.files == 4433 and ([.categories[]] | add) == 4433' \
  target/integration/w43-node-baseline/summary.json
```

Expected: `true`, exit 0.

- [x] **Step 4: Run the full Bun baseline on the same binary**

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_BASE_BIN="$W43_COMMON_DIR/w43/baseline/mbun"
python3 tools/integration/bun_corpus_runner.py \
  --bin "$W43_BASE_BIN" \
  --root "$PWD" \
  --cwd compat/bun \
  --discover compat/bun/test \
  --sample-per-group 100000 \
  --out target/integration/w43-bun-baseline \
  --jobs 4 \
  --timeout 30
```

Expected: `summary.json` exists and category sum equals 1,902.

- [x] **Step 5: Validate the Bun denominator and resource profile**

```bash
jq -e '.files == 1902 and ([.categories[]] | add) == 1902 and .resource_profile.memory_max == "4G" and .resource_profile.tasks_max == 512' \
  target/integration/w43-bun-baseline/summary.json
```

Expected: `true`, exit 0.

- [x] **Step 6: Write the baseline ledger and committed data record**

Create `.agents/docs/20260802-corpus-coverage-w43.md` with: base/head, binary relative path and SHA-256, exact commands, Node/Bun categories, raw rates, current excluded categories, audited runnable floor, elapsed time, and the statement that no source implementation has started. Update `compat/data/mbun-corpus-runs.json` so the new same-binary Node and Bun rows are contemporaneous and the stale note no longer describes the latest rows.

- [x] **Step 7: Verify and commit the baseline**

```bash
jq empty compat/data/mbun-corpus-runs.json
git diff --check
tools/integration/check_submodule_gitlinks.sh
git add .agents/docs/20260802-corpus-coverage-w43.md compat/data/mbun-corpus-runs.json
git commit --author='Sunrisepeak <speakshen@163.com>' \
  -m 'research(compat): freeze W43 corpus baseline' \
  -m '- #80' \
  -m 'Record same-binary Node and Bun full-corpus categories, provenance, and the audited runnable gap before source implementation.' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

### Task 3: Publish the plan and open the Draft PR

**Files:**
- Modify: `.agents/docs/20260802-corpus-coverage-w43-plan.md` only to check completed Task 1–2 boxes and record measured elapsed time.
- GitHub: Draft PR from `agent/corpus-coverage-w43` to `rewrite_bun_in_mcpp`.

**Interfaces:**
- Consumes: committed design, plan, and Task 2 baseline.
- Produces: Draft PR linked to #80 and checkpoint-0 comment containing only full baseline data.

- [x] **Step 1: Verify the already-published plan checkpoint**

```bash
git log --format=full -1 -- .agents/docs/20260802-corpus-coverage-w43-plan.md
git diff --check origin/agent/corpus-coverage-w43...HEAD
```

Expected: the plan commit contains #80, builder sign-off, and the Codex co-author trailer; diff check exits 0.

- [x] **Step 2: Push normally and create a Draft PR**

```bash
git push -u origin agent/corpus-coverage-w43
```

Create a Draft PR titled `compat: W43 five-hour Node/Bun corpus sprint`, base `rewrite_bun_in_mcpp`, head `agent/corpus-coverage-w43`. Its body links #80, names the raw/runnable denominators, distinguishes five logical lanes from three physical workers, includes the full baseline commands and counts, states +30 to +45 as the sprint target, and states that final 100% remains open.

- [x] **Step 3: Publish checkpoint 0**

Post one PR comment containing: base `163cb6d`, current head, binary SHA-256, full Node/Bun category tables, exact runnable gap, baseline elapsed time, Wave A targets, and current CI state as pending/not-started. Do not post worker probes as separate comments.

### Task 4: Create and gate the five exact Wave A worklists

**Files:**
- Create: `tools/integration/manifests/w43-a1-node-zlib-buffer.txt`
- Create: `tools/integration/manifests/w43-a2-node-assert.txt`
- Create: `tools/integration/manifests/w43-a3-node-permission.txt`
- Create: `tools/integration/manifests/w43-a4-bun-napi.txt`
- Create: `tools/integration/manifests/w43-a5-bun-test-runner.txt`

**Interfaces:**
- Consumes: Task 2 `results.tsv` files.
- Produces: five disjoint, baseline-confirmed non-green lists and five lane briefs with fixed targets.

- [x] **Step 1: Write A1 with the known zlib/Buffer validation frontier**

```text
compat/node/test/parallel/test-buffer-constants.js
compat/node/test/parallel/test-buffer-constructor-deprecation-error.js
compat/node/test/parallel/test-zlib-brotli-kmaxlength-rangeerror.js
compat/node/test/parallel/test-zlib-kmaxlength-rangeerror.js
compat/node/test/parallel/test-zlib-zstd-kmaxlength-rangeerror.js
```

Target: +3 to +5. Source boundary: `node_buffer_extra.cppm`, `node_zlib_iter.cppm`, `zlib_stream.cppm`, `runtime/zlib_stream.inc`.

- [x] **Step 2: Write A2 with the exact assert frontier**

```text
compat/node/test/parallel/test-assert-async.js
compat/node/test/parallel/test-assert-class-destructuring.js
compat/node/test/parallel/test-assert-class.js
compat/node/test/parallel/test-assert-deep-with-error.js
compat/node/test/parallel/test-assert-deep.js
compat/node/test/parallel/test-assert-first-line.js
compat/node/test/parallel/test-assert-partial-deep-equal.js
compat/node/test/parallel/test-assert-typedarray-deepequal.js
```

Target: +2 to +4. Source boundary: `node_assert_deepequal.cppm` plus the existing assert registration seam only.

- [x] **Step 3: Write A3 with the exact permission frontier**

```text
compat/node/test/parallel/test-permission-child-process-cli.js
compat/node/test/parallel/test-permission-config-file.mjs
compat/node/test/parallel/test-permission-fs-internal-module-stat.js
compat/node/test/parallel/test-permission-fs-read.js
compat/node/test/parallel/test-permission-fs-traversal-path.js
compat/node/test/parallel/test-permission-fs-write.js
compat/node/test/parallel/test-permission-net-fetch.js
compat/node/test/parallel/test-permission-net-udp.js
compat/node/test/parallel/test-permission-processbinding.js
compat/node/test/parallel/test-permission-sqlite-load-extension.js
```

Target: +2 to +4. Source boundary: `node_permission.cppm` and existing native permission gates. If Stage 1 proves more than one root cause, implement only the largest source-coherent group and leave the rest named in the lane result.

- [x] **Step 4: Write A4 with the N-API one-failure frontier**

```text
compat/bun/test/napi/napi-finalizer-delete-ref.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_bigint/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_dataview/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_exception/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_function/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_instance_data/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_new_target/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_number/do.test.ts
compat/bun/test/napi/node-napi-tests/test/js-native-api/test_typedarray/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_async/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_callback_scope/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_exception/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_fatal_exception/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_general/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_make_callback/do.test.ts
compat/bun/test/napi/node-napi-tests/test/node-api/test_null_init/do.test.ts
compat/bun/test/napi/uv.test.ts
compat/bun/test/napi/uv_stub.test.ts
```

Target: +3 to +5. Source boundary: `runtime/napi_core.inc`, `runtime/napi_objects.inc`, and `runtime/napi/*.h`; only one N-API worker exists in the wave.

- [x] **Step 5: Write A5 with the Bun test-runner one-failure frontier**

```text
compat/bun/test/cli/test/test-filter-lifecycle-snapshot.test.ts
compat/bun/test/js/bun/test/done-async.test.ts
compat/bun/test/js/bun/test/expect-extend-preload.test.ts
compat/bun/test/js/bun/test/fake-timers/sinonjs/fake-timers.test.ts
compat/bun/test/js/bun/test/mock/6874/A.test.ts
compat/bun/test/js/bun/test/mock/6874/B.test.ts
compat/bun/test/js/bun/test/only-failures.test.ts
compat/bun/test/js/bun/test/test-error-code-done-callback.test.ts
```

Target: +3 to +5. Source boundary: `src/test_runner.cppm`, `builtins/node_test_run.cppm`, `builtins/node_test_runner.cppm`, and `tests/test_test_runner.cpp`.

- [x] **Step 6: Intersect every manifest with the fresh baseline**

For Node manifests, every retained row must have classification `fail`, `timeout`, or `oom-kill` in `w43-node-baseline/results.tsv`. For Bun manifests, every retained row must be `test-failure`, `timeout`, `crash`, `load-error`, or `oom-kill` in `w43-bun-baseline/results.tsv`. Remove already-green or legitimately excluded paths before dispatch and record each removal in the W43 ledger.

- [x] **Step 7: Check retired approaches and overlap**

```bash
python3 tools/integration/check_struck.py --area node zlib buffer
python3 tools/integration/check_struck.py --area node assert
python3 tools/integration/check_struck.py --area node permission
python3 tools/integration/check_struck.py --area bun napi
python3 tools/integration/check_struck.py --area bun test runner
```

Expected: each output is copied into its lane brief. A struck result forbids repeating the named approach but does not hide the still-red file.

- [x] **Step 8: Commit the frozen Wave A worklists**

```bash
git diff --check
git add \
  tools/integration/manifests/w43-a1-node-zlib-buffer.txt \
  tools/integration/manifests/w43-a2-node-assert.txt \
  tools/integration/manifests/w43-a3-node-permission.txt \
  tools/integration/manifests/w43-a4-bun-napi.txt \
  tools/integration/manifests/w43-a5-bun-test-runner.txt \
  .agents/docs/20260802-corpus-coverage-w43.md
git commit --author='Sunrisepeak <speakshen@163.com>' \
  -m 'test(compat): freeze W43 Wave A worklists' \
  -m '- #80' \
  -m 'Record five disjoint baseline-confirmed lists, fixed targets, struck checks, and source ownership before worker dispatch.' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

### Task 5: Execute Wave A batch 1 — A1, A2, A3

**Files:**
- Worker A1: only its proven zlib/Buffer source/test paths.
- Worker A2: only assert builtin/test paths.
- Worker A3: only permission builtin/native-gate/test paths.

**Interfaces:**
- Consumes: frozen Task 2 binary and exact Task 4 manifests.
- Produces: three issue-linked worker branches, each with Red evidence, upstream source mapping, Green evidence, Refactor proof, and one reviewable commit.

- [x] **Step 1: Create three isolated worktrees from the same coordinator checkpoint**

Use `tools/integration/worktree_setup.sh` only on three new paths/branches: `w43/a1-zlib-buffer`, `w43/a2-assert`, and `w43/a3-permission`. Copy the exact manifest into each prompt; do not share a build target.

- [x] **Step 2: Dispatch three workers simultaneously**

Each worker first derives the same frozen binary path:

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_BASE_BIN="$W43_COMMON_DIR/w43/baseline/mbun"
test -x "$W43_BASE_BIN"
```

Worker A1 runs:

```bash
python3 tools/integration/node_corpus_runner.py \
  --bin "$W43_BASE_BIN" \
  --root "$PWD" \
  --files tools/integration/manifests/w43-a1-node-zlib-buffer.txt \
  --out target/integration/w43-a1-before \
  --jobs 1 \
  --timeout 30
```

Worker A2 runs the same command with manifest `w43-a2-node-assert.txt` and output `target/integration/w43-a2-before`. Worker A3 uses `w43-a3-node-permission.txt` and `target/integration/w43-a3-before`. Each worker reads the relevant pinned `compat/node/lib/` or `compat/node/src/` implementation before editing, creates or links the exact defect issue, implements one source-coherent cause, builds through `build_or_die.sh`, repeats the same manifest against the new binary, serially re-runs new greens, runs the relevant JSC member test, and commits with builder/co-author trailers.

- [x] **Step 3: Reject black-box or unproven results**

Reject a worker branch if its report lacks the upstream source location, a stable before failure, exact changed paths, after categories, serial new-green proof, or remaining red files. Do not accept a branch solely because its build passed.

### Task 6: Integrate and prove Wave A batch 1

**Files:**
- Modify only worker-proven source/tests and `.agents/docs/20260802-corpus-coverage-w43.md`.

**Interfaces:**
- Consumes: three reviewed worker commits from Task 5.
- Produces: one composed coordinator tree with zero focused regressions and measured batch-1 delta.

- [x] **Step 1: Review each branch before integration**

```bash
git diff --stat HEAD...w43/a1-zlib-buffer
git diff --stat HEAD...w43/a2-assert
git diff --stat HEAD...w43/a3-permission
git log --format=full -1 w43/a1-zlib-buffer
git log --format=full -1 w43/a2-assert
git log --format=full -1 w43/a3-permission
```

Confirm no worker modified `compat/`, shared metrics, protected surfaces, or another lane's source boundary.

- [x] **Step 2: Merge accepted branches normally with attributed merge commits**

```bash
GIT_AUTHOR_NAME=Sunrisepeak GIT_AUTHOR_EMAIL=speakshen@163.com \
git merge --no-ff w43/a1-zlib-buffer \
  -m 'merge(compat): integrate W43 A1 zlib buffer lane' \
  -m '- #80' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'

GIT_AUTHOR_NAME=Sunrisepeak GIT_AUTHOR_EMAIL=speakshen@163.com \
git merge --no-ff w43/a2-assert \
  -m 'merge(compat): integrate W43 A2 assert lane' \
  -m '- #80' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'

GIT_AUTHOR_NAME=Sunrisepeak GIT_AUTHOR_EMAIL=speakshen@163.com \
git merge --no-ff w43/a3-permission \
  -m 'merge(compat): integrate W43 A3 permission lane' \
  -m '- #80' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

Do not squash, cherry-pick, amend, or rebase; a rejected lane is omitted and reported.

- [x] **Step 3: Build the composed tree and derive impact lists**

```bash
W43_A_BIN=$(bash tools/integration/build_or_die.sh)
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_WAVE_A_BIN="$W43_COMMON_DIR/w43/wave-a/mbun"
install -Dm755 "$W43_A_BIN" "$W43_WAVE_A_BIN"
python3 tools/integration/impact_gate.py \
  --rev-range 08a4de9..HEAD \
  --node-run target/integration/w43-node-baseline \
  --bun-run target/integration/w43-bun-baseline \
  --out target/integration/w43-wave-a-node-impact.txt \
  --bun-out target/integration/w43-wave-a-bun-impact.txt \
  --explain
```

- [x] **Step 4: Run composed batch-1 focused and member gates**

Run A1, A2, and A3 separately against `"$W43_WAVE_A_BIN"` with the Node runner, their committed manifest, `--jobs 3`, `--timeout 30`, and distinct `target/integration/w43-a*-composed` output directories. Then repeat every newly green file with `--jobs 1`. Run:

```bash
tools/integration/build_lock.sh mcpp test -p jsc
python3 tools/integration/corpus_diff.py target/integration/w43-a1-before target/integration/w43-a1-composed --json
python3 tools/integration/corpus_diff.py target/integration/w43-a2-before target/integration/w43-a2-composed --json
python3 tools/integration/corpus_diff.py target/integration/w43-a3-before target/integration/w43-a3-composed --json
```

Any stable green→non-green result blocks batch 2.

### Task 7: Execute Wave A batch 2 — A4 and A5

**Files:**
- Worker A4: N-API runtime/test boundary only.
- Worker A5: test-runner builtin/member-test boundary only.

**Interfaces:**
- Consumes: Task 6 composed checkpoint and A4/A5 manifests.
- Produces: two issue-linked, source-driven branches with combined target +6 to +10.

- [x] **Step 1: Create two new worktrees on the Task 6 checkpoint**

Branches: `w43/a4-bun-napi` and `w43/a5-bun-test-runner`.

- [x] **Step 2: Dispatch both workers simultaneously**

Each worker derives the shared pre-edit binary and A4 runs:

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_WAVE_A_BIN="$W43_COMMON_DIR/w43/wave-a/mbun"
python3 tools/integration/bun_corpus_runner.py \
  --bin "$W43_WAVE_A_BIN" \
  --root "$PWD" \
  --cwd compat/bun \
  --list tools/integration/manifests/w43-a4-bun-napi.txt \
  --out target/integration/w43-a4-before \
  --jobs 1 \
  --timeout 30
```

A5 runs the same command with list `w43-a5-bun-test-runner.txt` and output `target/integration/w43-a5-before`. Each worker reads the relevant Bun/Node N-API or Bun test-runner upstream source before editing, creates/links the defect issue, implements one coherent cause, builds, repeats its list, serially proves new greens, runs the relevant JSC member test, and commits.

- [x] **Step 3: Apply the same evidence rejection gate as Task 5**

No upstream mechanism, no stable red, no serial proof, or any `compat/` edit means rejection.

### Task 8: Integrate Wave A and publish checkpoint 1

**Files:**
- Modify: `.agents/docs/20260802-corpus-coverage-w43.md`
- Modify: `changelog.md` in the same substantive integration commit when useful.

**Interfaces:**
- Consumes: accepted A4/A5 commits plus Task 6 tree.
- Produces: full Wave A focused evidence, actual +N versus target +12 to +20, zero-regression verdict, and one PR comment.

- [x] **Step 1: Review and merge A4 then A5 normally**

Run branch diff/trailer checks and ensure source boundaries are disjoint, then:

```bash
GIT_AUTHOR_NAME=Sunrisepeak GIT_AUTHOR_EMAIL=speakshen@163.com \
git merge --no-ff w43/a4-bun-napi \
  -m 'merge(compat): integrate W43 A4 Bun N-API lane' \
  -m '- #80' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'

GIT_AUTHOR_NAME=Sunrisepeak GIT_AUTHOR_EMAIL=speakshen@163.com \
git merge --no-ff w43/a5-bun-test-runner \
  -m 'merge(compat): integrate W43 A5 Bun test runner lane' \
  -m '- #80' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

- [x] **Step 2: Build and run the five-manifest union**

```bash
W43_WAVE_A_FINAL_BIN=$(bash tools/integration/build_or_die.sh)
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_WAVE_B_BASE_BIN="$W43_COMMON_DIR/w43/wave-b-base/mbun"
install -Dm755 "$W43_WAVE_A_FINAL_BIN" "$W43_WAVE_B_BASE_BIN"
sha256sum "$W43_WAVE_B_BASE_BIN"
```

Run A1–A3 with the Node runner and A4–A5 with the Bun runner, at `--jobs 3`; serially repeat every claimed new green. Run `tools/integration/build_lock.sh mcpp test -p jsc` and the diff-derived impact lists.

- [x] **Step 3: Record target versus actual**

Append lane target, actual green delta, remaining classifications, issue, commit, member-test result, serial proof count, and elapsed time to the W43 ledger. State `Wave A target missed` if net delta is below 12; do not lower the target after observing results.

- [x] **Step 4: Push and publish one checkpoint comment**

The comment contains source issues/commits, five lane targets→actuals, cumulative new greens, zero-regression result, timeout/OOM movement in the focused sets, physical concurrency, blockers, and the exact Wave B selection command. It explicitly says no post-Wave-A full corpus was run.

### Task 9: Materialize Wave B from the measured frontier

**Files:**
- Create: five `tools/integration/manifests/w43-b*.txt` files from the Task 2 baseline after excluding Wave A paths.
- Modify: `.agents/docs/20260802-corpus-coverage-w43.md` with the literal assignments and targets.

**Interfaces:**
- Consumes: Task 2 full baseline plus Task 8 integrated outcomes.
- Produces: five literal, disjoint manifests assigned to REPL, combined crypto/webcrypto, Bun third-party, Bun CLI/run, and Node fs; combined target +12 to +22.

- [x] **Step 1: Run the ranked planner with Wave A areas excluded**

```bash
python3 tools/integration/wave_planner.py \
  --node-run target/integration/w43-node-baseline \
  --bun-run target/integration/w43-bun-baseline \
  --plan 5 \
  --lane-hours 1.3 \
  --min-actionable 2 \
  --min-per-corpus 1 \
  --exclude zlib \
  --exclude buffer \
  --exclude assert \
  --exclude permission \
  --exclude napi \
  --exclude test-runner
```

- [x] **Step 2: Select only the spec-approved disjoint owners**

Use this priority order: Node REPL, one combined crypto+webcrypto owner, Bun `js/third_party`, Bun `cli/run`, Node fs. If the planner reports fewer than two actionable files for one owner, take the next ranked owner that does not overlap another source touch-set. Record the literal replacement and planner evidence in the ledger before dispatch.

- [x] **Step 3: Write literal manifests from baseline rows**

Sort each owner's non-green rows by failed-assertion ratio and stable duration, cap each manifest at 12 files, exclude Wave A paths and struck approaches, then write the exact paths to `w43-b1` through `w43-b5`. Run every manifest against the integrated pre-Wave-B binary with `--jobs 1`; remove any now-green path and record it as an inherited Wave A gain rather than Wave B credit.

- [x] **Step 4: Lock targets before workers start**

Assign +2 to +5 per Node lane and +2 to +4 per Bun lane, totaling +12 to +22. Targets are written to the ledger and PR checkpoint comment before implementation; they are not changed after results arrive.

- [ ] **Step 5: Commit the literal Wave B manifests before dispatch**

```bash
git diff --check
git add tools/integration/manifests/w43-b1-repl.txt \
  tools/integration/manifests/w43-b2-crypto-webcrypto.txt \
  tools/integration/manifests/w43-b3-bun-third-party.txt \
  tools/integration/manifests/w43-b4-bun-cli-run.txt \
  tools/integration/manifests/w43-b5-node-fs.txt \
  .agents/docs/20260802-corpus-coverage-w43.md
git commit --author='Sunrisepeak <speakshen@163.com>' \
  -m 'test(compat): freeze W43 Wave B worklists' \
  -m '- #80' \
  -m 'Record the measured post-Wave-A assignments, literal paths, fixed targets, and disjoint source ownership.' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

### Task 10: Execute and integrate Wave B in two rolling batches

**Files:**
- Worker source/test boundaries determined by the five literal Task 9 briefs.
- Modify: `.agents/docs/20260802-corpus-coverage-w43.md`
- Modify: `changelog.md`

**Interfaces:**
- Consumes: five Task 9 manifests/briefs and integrated Wave A binary.
- Produces: accepted B1–B5 source commits, composed build, focused +12 to +22 target evidence, and checkpoint-2 PR comment.

- [ ] **Step 1: Run B1–B3 with three workers**

Create isolated worktrees from the manifest commit on branches `w43/b1-repl`, `w43/b2-crypto-webcrypto`, and `w43/b3-bun-third-party`. Each worker must:

1. derive and verify `"$W43_WAVE_B_BASE_BIN"` under the Git common directory;
2. run its literal manifest at `--jobs 1` into a lane-specific `*-before` directory and preserve the stable-red classifications;
3. read the corresponding pinned Node/Bun upstream implementation and record the exact mechanism/source locations before editing;
4. create or link the narrow defect issue, then change only its assigned source/test boundary;
5. build through `build_or_die.sh`, rerun the literal manifest into `*-after`, and serially repeat every claimed new green;
6. run the relevant JSC member test through `build_lock.sh`, record all remaining red files, and commit with #80, builder sign-off, and Codex co-author trailers.

Reject any lane that widens beyond its manifest/source boundary, edits `compat/`, or lacks stable Red, upstream mapping, Green delta, serial proof, and remaining-red evidence.

- [ ] **Step 2: Review, merge, build, and focused-gate B1–B3**

Review each branch diff and full commit trailers, then merge each with `--no-ff`; the subjects are respectively `merge(compat): integrate W43 B1 lane`, `merge(compat): integrate W43 B2 lane`, and `merge(compat): integrate W43 B3 lane`. Every body references #80 and includes builder sign-off plus Codex co-author trailer. Then run a single coordinator build, run all three literal manifests at `--jobs 3` into distinct composed-result directories, serially repeat every new green at `--jobs 1`, run the relevant member tests through `build_lock.sh`, derive impact lists with `impact_gate.py`, and compare each before/composed pair with `corpus_diff.py`.

- [ ] **Step 3: Run B4–B5 with two workers from the accepted B1–B3 checkpoint**

Use branches `w43/b4-bun-cli-run` and `w43/b5-node-fs`. For each lane, verify the shared pre-edit binary; capture a single-job stable Red run; map the failure to pinned upstream source; create/link the defect issue; implement only that source-coherent mechanism; build; capture the after run; serially prove every new green; run the relevant member test through `build_lock.sh`; record remaining reds; and commit with #80, builder sign-off, and Codex co-author trailers. Reject any `compat/` edit, unproven source mapping, boundary widening, or missing before→after/serial evidence.

- [ ] **Step 4: Integrate and prove all five B lanes**

Merge B4 and B5 with attributed `--no-ff` merge commits whose bodies reference #80 and include builder sign-off plus Codex co-author trailer. Build once, run every B1–B5 manifest into its own composed-result directory at `--jobs 3`, serially repeat all claimed B greens at `--jobs 1`, run `tools/integration/build_lock.sh mcpp test -p jsc`, and compare every before/composed pair against the frozen pre-Wave-B results with `corpus_diff.py`.

- [ ] **Step 5: Publish checkpoint 2**

Push normally. Post one comment with B1–B5 targets→actuals, cumulative Wave A+B delta, focused zero-regression result, elapsed time, resource changes, failed/rejected lanes, and the explicit statement that final full corpus verification is still pending.

### Task 11: Run final same-binary full verification

**Files:**
- Evidence only: `target/integration/w43-node-candidate/`
- Evidence only: `target/integration/w43-bun-candidate/`
- Modify: `compat/data/mbun-corpus-runs.json`
- Modify: `README.md`
- Modify: `changelog.md`
- Modify: `.agents/docs/20260802-corpus-coverage-w43.md`

**Interfaces:**
- Consumes: composed Wave A+B source tree.
- Produces: authoritative before→after proof, final sprint verdict, remaining runnable gap, and recalibrated ETA.

- [ ] **Step 1: Stop new dispatch and build one final binary**

```bash
W43_FINAL_BIN=$(bash tools/integration/build_or_die.sh)
W43_FINAL_SHA=$(sha256sum "$W43_FINAL_BIN" | awk '{print $1}')
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_FINAL_FROZEN_BIN="$W43_COMMON_DIR/w43/final/mbun"
install -Dm755 "$W43_FINAL_BIN" "$W43_FINAL_FROZEN_BIN"
test "$(sha256sum "$W43_FINAL_FROZEN_BIN" | awk '{print $1}')" = "$W43_FINAL_SHA"
tools/integration/safe-test.sh 10 "$W43_FINAL_BIN" --version
```

- [ ] **Step 2: Run Node then Bun full candidate suites**

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_FINAL_FROZEN_BIN="$W43_COMMON_DIR/w43/final/mbun"
test -x "$W43_FINAL_FROZEN_BIN"

python3 tools/integration/node_corpus_runner.py \
  --bin "$W43_FINAL_FROZEN_BIN" \
  --root "$PWD" \
  --out target/integration/w43-node-candidate \
  --jobs 4 \
  --timeout 15

python3 tools/integration/bun_corpus_runner.py \
  --bin "$W43_FINAL_FROZEN_BIN" \
  --root "$PWD" \
  --cwd compat/bun \
  --discover compat/bun/test \
  --sample-per-group 100000 \
  --out target/integration/w43-bun-candidate \
  --jobs 4 \
  --timeout 30
```

Do not resume from baseline directories. Both suites must consume the same frozen SHA-256 captured in Step 1.

- [ ] **Step 3: Gate denominators and regressions**

```bash
jq -e '.files == 4433 and ([.categories[]] | add) == 4433' target/integration/w43-node-candidate/summary.json
jq -e '.files == 1902 and ([.categories[]] | add) == 1902' target/integration/w43-bun-candidate/summary.json
python3 tools/integration/corpus_diff.py target/integration/w43-node-baseline target/integration/w43-node-candidate --json
python3 tools/integration/corpus_diff.py target/integration/w43-bun-baseline target/integration/w43-bun-candidate --json
```

Expected: both denominator checks exit 0 and both diffs report zero stable green→non-green regressions. Any regression blocks a positive sprint verdict.

- [ ] **Step 4: Compute the sprint verdict without relabeling scope**

Calculate Node pass delta, Bun green delta, combined delta, skip/exclusion movement, timeout/crash/OOM movement, and audited runnable denominator. Verdicts are exactly: `target met` for +30 to +45 with all gates; `target exceeded` above +45 with all gates; `target missed` below +30; `regressed` for any stable green loss or timeout/OOM increase.

- [ ] **Step 5: Synchronize committed evidence**

Update README only from final full summaries. Replace stale latest rows in `mbun-corpus-runs.json` with same-binary W43 baseline and final entries including commands, hashes, resource profile, and notes. Update changelog and W43 ledger with full before→after and the fact that 100% remains incomplete unless the audited runnable denominator is fully green.

- [ ] **Step 6: Verify and commit the final evidence**

```bash
jq empty compat/data/mbun-corpus-runs.json
git diff --check
tools/integration/check_submodule_gitlinks.sh
git add README.md compat/data/mbun-corpus-runs.json changelog.md .agents/docs/20260802-corpus-coverage-w43.md
git commit --author='Sunrisepeak <speakshen@163.com>' \
  -m 'docs(compat): publish W43 measured corpus result' \
  -m '- #80' \
  -m 'Synchronize the same-binary full Node/Bun before-after evidence, sprint verdict, remaining runnable gap, and recalibrated ETA.' \
  -m 'Signed-off-by: Sunrisepeak <speakshen@163.com>' \
  -m 'Co-authored-by: Codex (GPT-5) <>'
```

### Task 12: Final PR synchronization and CI audit

**Files:**
- GitHub Draft PR conversation and metadata.

**Interfaces:**
- Consumes: final evidence commit and full summary files.
- Produces: one final sprint checkpoint comment and an evidence-backed CI state; PR remains Draft unless all required scope gates are complete.

- [ ] **Step 1: Push final head normally**

```bash
git push origin agent/corpus-coverage-w43
```

- [ ] **Step 2: Publish the final sprint checkpoint**

Post one comment with full Node/Bun category tables, raw and runnable before→after rates, exact +N, timeout/crash/OOM movement, zero-regression result, elapsed wall time, accepted/rejected lane table, final head SHA, remaining gap, and recalculated 100% ETA. State every unmet original requirement explicitly.

- [ ] **Step 3: Observe final-head CI only**

Use `gh pr checks` and Actions run/job output for the final head. Do not treat running, queued, cancelled, superseded, or earlier-head jobs as success. Record GCC 16.1.0, LLVM 22.1.8, and macOS probe separately.

- [ ] **Step 4: Completion audit**

Check issue #80, design §1/§6/§10, and every task above requirement-by-requirement. The sprint may be reported as a measured checkpoint while the thread goal remains active. Mark the overall 100% goal complete only if current full summaries prove the entire audited runnable denominator green and no required work remains.

## Long-range continuation after the five-hour checkpoint

After Task 12, repeat the same two-wave cycle from the new full summary. Recompute the trailing-three-wave net rate and remaining audited gap after every final full run. At the provisional 6–9 net files/hour, the initial 1,524-file runnable gap implies 170–255 coordinator wall-hours or 34–51 five-hour sprints; this estimate must move with evidence and is never used to claim completion early.
