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
- `reclaim_disk.sh` — reclaims regenerable build caches from **stale** agent
  worktrees: `reclaim_disk.sh --apply`. Dry-run by default. The bounded layer
  already refuses to start a measurement on a nearly-full disk
  (`ensure_disk_headroom()`), and that guard is correct — but on 2026-07-29 the
  box reached **100% with 6.1 GB free of 1.5 TB**, which would have aborted every
  run of the wave with an error that reads like a runner bug. The cause is
  structural: each parallel round leaves worktrees behind, each accumulates a
  `target/` *plus* per-member `modules/*/target/` trees, and nobody owns them
  (`.claude/worktrees/wt4` alone held 11 GB under `modules/jsc/target`; five
  stale round-2/round-9 worktrees held 49 GB). Reclaiming only those caches
  restored 44 GB without touching a line of source. **Staleness is by mtime, not
  by "is it the current worktree"** — the first cut of the script protected only
  the current checkout and its dry run promptly offered to delete the five
  worktrees of the wave then executing. A cache touched within `--stale-hours`
  (default 24) belongs to somebody, and no agent can see who. Never reclaims
  source files, the current worktree, or the shared `~/.mcpp/bmi`.
- `check_conflict_markers.sh` — fails if a tracked file still carries an
  unresolved merge-conflict marker. **Not redundant with the compiler:** mbun's
  builtins embed JavaScript inside C++ raw string literals, so a marker left in
  that JS is ordinary text to the C++ compiler — it compiles clean, links clean,
  and ships a binary whose `execSync` is broken at runtime. That happened during
  the round-9 integration: `mcpp build` reported "Finished release [optimized]"
  over a file holding four markers. A green build is not evidence a merge was
  resolved; run this as the last step of any conflict resolution.
### Build hazards that silently produce a stale binary

Three separate failure modes have now cost measurement time. All of them look
like success:

- **A new or edited `.cppm` is not always picked up.** `mcpp build` reports
  `Finished release in 0.01s` over changed sources; a brand-new module partition
  instead fails with `failed to read compiled module: <partition>.gcm`. Cause:
  the generated `build.ninja` is keyed on the module list at generation time.
  Cheap fix: `touch mcpp.toml modules/<member>/mcpp.toml`, or
  `find target -name build.ninja -delete`. `worktree_setup.sh` does the latter
  on every re-point.
- **The root build does not notice a dependency member's `.cpp`** (only its
  `.cppm`). Editing `modules/tls/src/openssl.cpp` produced `Finished in 2.66s`
  with nothing recompiled — caught only because the measurement came back
  byte-identical. Touch the member's `.cppm` to force it.
- **`mcpp clean --bmi-cache` is not local.** It wipes the worktree's whole
  `target/` *and* the **shared `~/.mcpp/bmi`**, so every other worktree on the
  machine eats a cold rebuild. Never use it while other agents are running;
  prefer the `touch` above.

`build_lock.sh` compares source mtimes against the binary it just produced and
warns when sources are newer — **if you see that warning, do not measure**, because
the run would score the previous binary.

- `latency_probe.py` — asserts an *order of magnitude* on calls that should be
  trivially fast, so a catastrophic slowdown fails a check instead of hiding in
  the corpus's timeout bucket. Written after `dns.lookup` was found taking
  **8011 ms** (3 ms after the fix) — a 2670x defect that moved no compatibility
  number, because the runner reports pass/fail and an 8-second call only shows up
  once it crosses a 15-second timeout, where it reads as a hang rather than as
  latency. `--save` records a baseline; `--baseline` gates on a multiple-x
  slowdown, which catches regressions while the absolute number is still small
  and a fixed threshold is blind. Exit 1 = too slow, exit 2 = a probe could not
  run at all. **Documented limit:** it does not reproduce that dns case itself —
  see the module docstring for the two probe designs that failed to discriminate
  and why; those corpus files guard it instead.
- `hang_dump.js` — answers *"why is this corpus file hanging?"* without a rebuild:
  `safe-test.sh 30 <mbun> tools/integration/hang_dump.js <abs-path-to-test> [ms]`.
  It `require`s the target (so its own timer shares the loop), then dumps the
  reactor's `pending`/`handles`/`serveActive`/`stall` plus every registered item
  with `_fd`/`_refd`/`_held`/`destroyed`/`listening`/`_wq`, and every timer with
  its source text. Exit 99 marks a dump rather than the test's own exit. It works
  because its timer is **ref'd**, which bounds the pump's otherwise 60-second
  `poll()` park so control returns to JS while the hang is still in progress.
  **Why it matters:** the runner classifies a hang as `timeout`, which says
  nothing about the cause, and 46 hangs had gone uninvestigated across several
  rounds because looking inside one was expensive. With this, all 46 were triaged
  in a session — and the result overturned the standing assumption: **31 of 41
  survivors are not event-loop bugs at all** but protocol-semantics gaps wearing
  a hang costume (20 are a stalled exchange where the test is genuinely waiting;
  11 are a server outliving a dead client flow). 4 produce no dump at all, which
  is itself the finding: the pump is blocked inside native code and JS never
  regains control.
- `smoke_examples.py` — boots each `examples/` app in turn, requests
  `http://127.0.0.1:3000/`, asserts a 2xx, then reaps the whole process tree
  (`bounded_run.BoundedServer`). Sequential by design: the demos all hardcode
  port 3000. `--app <name>` runs one; CI runs the full set on the gcc lane.
  An app whose `node_modules` is missing is reported as `skipped-no-deps`
  unless `--install` is passed (bootstrapping through `mbun install` does not
  currently finish for the framework demos).

## The strategy loop

Coverage work is driven by a loop, not by judgement calls that live in one
session's head. Four artifacts, each with a self-test:

| artifact | role |
| --- | --- |
| `wave_planner.py` | reads corpus state + ledger + struck registry + live resources, emits ranked lane assignments with a goal each |
| `lane_ledger.tsv` | one row per lane, appended only when the result is **integrator-verified**; the throughput model's only input |
| `struck.tsv` | areas/targets already retired, with the measured cost that retired them |
| `check_struck.py` | run before dispatching a lane; exits 3 on a hit |

```bash
# where does 100% actually stand, and what blocks the rest?
python3 tools/integration/wave_planner.py --coverage \
  --node-run target/integration/<node-run> --bun-run target/integration/<bun-run>

# what do past lanes say a lane can deliver per hour?
python3 tools/integration/wave_planner.py --throughput

# plan the next wave (refuses if the box cannot afford it)
python3 tools/integration/wave_planner.py --plan 5 \
  --node-run target/integration/<node-run> --bun-run target/integration/<bun-run>

# before dispatching each assignment
python3 tools/integration/check_struck.py <area terms>
```

Three properties worth knowing, because each exists in response to something
that actually went wrong:

- **Goals come from measured throughput, not a constant.** Observed rates spanned
  25x across two waves (1.4 to 15.0 files/hour), so a flat `+6` was simultaneously
  trivial for one area and unreachable for another.
- **`--coverage` separates *actionable* failures from *no-verdict* ones**
  (timeout / oom / self-skip / environment-blocked) and from struck areas, then
  states the ceiling if every actionable file landed. Folding timeouts into
  "fixable" is how a subsystem's density gets overstated.
- **The resource guard refuses rather than overcommits.** The disk has hit 100%
  twice, and measuring next to four other lanes turned 113 real failures into 366
  phantom ones — so a plan reports its disk/memory/cpu budget, caps at 5 lanes,
  and exits non-zero with the remedy named when it cannot afford the wave.

Two measurement rules the planner prints into every plan, both learned the hard
way and both cheap to follow:

1. **A BEFORE must correspond to your own branch point** — a frozen run directory
   whose tree you know, or a build of your own parent commit. Never rebuild to
   manufacture a baseline; that was the single largest time sink measured.
2. **A parallel run is a screen, never a verdict.** Re-run any file whose state
   decides a number serially (`--jobs 1`) before believing it.

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
bash tools/integration/tests/test_reclaim_disk.sh
bash tools/integration/tests/test_latency_probe.sh
bash tools/integration/tests/test_wave_planner.sh
bash tools/integration/tests/test_check_struck.sh
bash tools/integration/tests/test_tick_order_gate.sh
bash benchmarks/tools/test-bench3.sh
```

## Adding a new tool

Reuse, don't re-derive: import `bounded_run` (Python) or call `safe-test.sh`
(shell). If a new failure mode gets past the layer, fix it **in the layer** and
note the incident in the docstring, so every tool inherits the protection.
