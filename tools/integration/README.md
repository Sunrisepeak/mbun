# tools/integration — bounded execution for tests, benches, and debugging

Everything here shares one rule: **workloads that execute tests, benchmarks,
or debug targets never run bare.** They go through the shared safety layer so
a hostile workload degrades into a killed scope and a clear log — never a
frozen machine or a silently murdered harness.

## The safety layer

- `bounded_run.py` — Python module used by every runner. One
  `BoundedRun(log).run(cmd, timeout)` gives you all of:
  1. systemd `--user` scope: `MemoryMax=4G`, `MemorySwapMax=0`, `TasksMax=512`,
     `RuntimeMaxSec` — a fork-deadlock OOM-kills inside the scope instead of
     swap-thrashing the box;
  2. `TimeoutStopSec=3` — SIGKILL follows RuntimeMaxSec's SIGTERM, so children
     that ignore SIGTERM (node `child_process` suites) still die;
  3. `start_new_session` — tests that signal their whole process group
     (`kill(0, SIGABRT)`) cannot kill the harness (this silently killed three
     corpus runs before it was isolated);
  4. stdout straight to the log file, never a pipe — orphaned grandchildren
     holding a pipe stall `subprocess.run()` past its timeout and wedge the
     worker pool;
  5. private per-test `TMPDIR`, deleted afterwards — killed tests leak tmpdir
     debris (one corpus round left ~500k entries in `/tmp` and filled the
     disk);
  6. `ensure_disk_headroom()` — refuse to start when the disk is nearly full.
- `safe-test.sh` — the same protections for ad-hoc shell use:
  `tools/integration/safe-test.sh <timeout_sec> <cmd...>`. ALWAYS use it for
  one-off runs of spawn-heavy tests; `benchmarks/tools/*.sh` route every
  runner invocation through it.

## Runners

- `bun_corpus_runner.py` — bun's native test corpus (`compat/bun/test`)
  through `mbun test`, with per-file classification (green / test-failure /
  timeout / oom-kill / …). See `compat/README.md` for the measurement recipe.
- `node_corpus_runner.py` — node's `compat/node/test/parallel` executed
  directly (`mbun <file>`, exit 0 = pass); node-harness-dependent files count
  as failures, keeping the number honest file-level coverage. A file that
  skipped itself (`common.skip()` → `1..0 # Skipped:`, exit 0) is classified
  `skipped`, never `pass`: it exits clean precisely because the runtime lacks
  what it wanted to test, so counting it would reward the gap it reports (277
  of the round-8 run's 1810 "passes" were skips; all 237 `test-quic-*` skip on
  `!process.features.quic`). `--files` / `--filter` scope a run to one cluster.
  `--resume` keeps what a previous `--out` already recorded and runs only the
  rest; `--max-seconds` stops dispatching once a wall-clock budget is spent,
  writes what finished, and marks the summary `incomplete` with a `remaining`
  count. Together they complete a full 4433-file measurement across several
  bounded invocations — a single run outlives an agent turn and used to lose
  everything when killed at that boundary. The budget is checked before
  dispatching a file, never mid-file, so it can never manufacture a timeout.
- `test_members.sh` — `mcpp test` across every workspace member.
- `corpus_diff.py` — compares two corpus rounds' `results.tsv` and **gates on
  regressions**: `corpus_diff.py <before-dir> <after-dir>`. Prints per-bucket
  before→after deltas, the files that went green→non-green (REGRESSIONS) and
  non-green→green (gains), and the largest assertion-level moves among files
  that stayed non-green. Exits non-zero on any green→non-green regression so it
  can gate a merge; `--allow-regressions <manifest>` excuses known-flaky files
  (fnmatch patterns, same shape as the runner's blocked manifest — e.g. the
  network-dependent `hosted-git-info`). `--json` emits a machine summary;
  `--perf` additionally flags files whose `duration_ms` grew past
  `--perf-threshold` (default 1.5×). Replaces the ad-hoc python one-liners that
  kept mis-diffing rounds via cwd-drift and stale baselines.
- `cluster_finder.py` — turns a corpus run into a ranked, actionable work-list.
  It does not execute anything; it reads a runner's output dir
  (`node_corpus_runner.py --out <dir>`: `results.tsv` + `logs/*.log`) and buckets
  the non-green, non-timeout files by `(subsystem, normalised-error-signature)`,
  ranking subsystems by *fixable* density so a round targets the densest
  single-root-cause cluster instead of a scattered guess. Volatile bits (paths,
  numbers, quoted literals, hex) are scrubbed from each error line so one root
  cause collapses into one bucket. Timeouts (usually child_process harness gaps)
  are reported separately and never inflate a cluster. `--filter test-vm` scopes
  to one subsystem; `--json` emits a machine summary. Analysis-only, so it needs
  no bounded layer of its own — all execution stayed in the runner.
  It also ranks signatures **across** subsystems and prints that section first:
  the causes costing the most files are usually not subsystem-specific, and
  per-subsystem grouping alone shreds them into shards that never reach the top
  (node's harness flag re-spawn — 848 files, 19% of the corpus, one cause — hid
  behind a `quic: 234` row for several rounds). `--min-subsystems` sets how wide
  a cause must spread to count as cross-cutting. `--worklist <path>` writes the
  chosen cluster out as a runner `--files` list (`--worklist-rank` /
  `--worklist-from subsystem` select which), so picking a round's target and
  scoring it before/after are the same set of files rather than two hand-copied
  approximations.
- `worktree_setup.sh` — creates or re-points a parallel-agent worktree with the
  compat corpora wired: `worktree_setup.sh <path> <branch> [start-point]`.
  `compat/{bun,node}` are multi-GB submodules a worktree must not re-materialise,
  so they are symlinked at the main checkout's copies — and doing that by hand is
  a trap. git leaves an **empty** submodule directory in a fresh worktree, so
  `ln -sfn <target> <wt>/compat/node` silently links *inside* it
  (`compat/node/node -> …`), the corpus path stops resolving, and the failure
  surfaces far away as "no test files selected" or as a runner scoring a subset
  it never ran. The script replaces symlinks and empty dirs, repairs a
  previously mis-wired worktree, refuses to touch a populated one, preserves
  `target/` (the incremental build cache), and *proves* the wiring by resolving
  the corpus before it returns.
- `build_lock.sh` — serialises expensive builds **across** parallel agent
  worktrees: `build_lock.sh mcpp build`. `mcpp build`'s link phase spawns many
  `ld` processes at ~1 GB RSS each; one build is fine on this box, three
  overlapping ones are not (a measured round-9 peak hit load 41, drove available
  memory from ~40 GB to 19 GB and filled the 2 GB swapfile — one step short of
  the freeze the whole bounded layer exists to prevent, arriving through the
  build rather than through a test). Per-agent discipline cannot fix it because
  no agent can see the other worktrees; the lock lives in the shared
  `--git-common-dir`, so it spans every linked worktree and every agent process.
  `MBUN_BUILD_SLOTS` (default 1) sets how many may run at once,
  `MBUN_BUILD_WAIT` how long to wait before giving up with exit 75 — it never
  runs the command after giving up.
- `check_conflict_markers.sh` — fails if a tracked file still carries an
  unresolved merge-conflict marker. **Not redundant with the compiler:** mbun's
  builtins embed JavaScript inside C++ raw string literals, so a marker left in
  that JS is ordinary text to the C++ compiler — it compiles clean, links clean,
  and ships a binary whose `execSync` is broken at runtime. That happened during
  the round-9 integration: `mcpp build` reported "Finished release [optimized]"
  over a file holding four markers. A green build is not evidence a merge was
  resolved; run this as the last step of any conflict resolution.
- `smoke_examples.py` — boots each `examples/` app in turn, requests
  `http://127.0.0.1:3000/`, asserts a 2xx, then reaps the whole process tree
  (`bounded_run.BoundedServer`). Sequential by design: the demos all hardcode
  port 3000. `--app <name>` runs one; CI runs the full set on the gcc lane.
  An app whose `node_modules` is missing is reported as `skipped-no-deps`
  unless `--install` is passed (bootstrapping through `mbun install` does not
  currently finish for the framework demos).

## Self-tests

Every tool has a self-test under `tests/`; run them after touching a runner:

```bash
bash tools/integration/tests/test_bun_corpus_runner.sh
bash tools/integration/tests/test_node_corpus_runner.sh
bash tools/integration/tests/test_corpus_diff.sh
bash tools/integration/tests/test_cluster_finder.sh
bash tools/integration/tests/test_smoke_examples.sh
bash tools/integration/tests/test_worktree_setup.sh
bash tools/integration/tests/test_build_lock.sh
bash tools/integration/tests/test_check_conflict_markers.sh
bash benchmarks/tools/test-bench3.sh
```

## Adding a new tool

Reuse, don't re-derive: import `bounded_run` (Python) or call `safe-test.sh`
(shell). If a new failure mode gets past the layer, fix it **in the layer** and
note the incident in the docstring, so every tool inherits the protection.
