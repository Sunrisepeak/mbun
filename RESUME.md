# RESUME — where the corpus push stands

Written by the integration agent after every wave and before every dispatch, so a
session that is interrupted (usage limit, crash, restart) can pick up from the
file rather than from memory. **If you are a fresh session reading this, start
here.**

Session-scoped cron jobs are in-memory only (`durable` has no effect), so the
hourly loop survives a usage limit — it simply skips the fires that land during
the block and resumes within an hour — but it does NOT survive the session
ending. This file is what makes that recoverable.

## 2026-07-29 continuation checkpoint

This section supersedes the older state snapshot below.

- **Integration branch**: `agent/corpus-coverage-live`, based linearly on
  `rewrite_bun_in_mcpp` at `5a901d7`.
- **Current published baseline**: node `2,654 / 4,433` pass and bun
  `868 / 1,902` green, from the same target-branch build recorded by PR #32.
- **PR**: the live draft PR replaces blocked PR #33; do not reopen or reuse
  merged PR #25.

The first three-way reuse probe tested local round-10 branches for node's
`--test` CLI, HTTP/2 argument/timer validation, and verbatim Node error text.
All three candidate cherry-picks became empty on the current target tree.
Fresh targeted measurements confirmed that the named historical acceptance
sets were already absorbed:

- `test-runner-*`: current `28 pass / 43 fail / 3 skipped / 3 timeout` (77
  files), already beyond the candidate's historical `26 / 77`;
- the HTTP/2 candidate's eight named files: `8 / 8` pass;
- the error-text candidate's four named files: `4 / 4` pass.

**Dispatch correction:** branch ancestry and `git cherry` are not sufficient
evidence that an old agent result is still missing. The target branch may have
absorbed the same semantics through a differently shaped later commit. Before
allocating a build slot to historical work, require both:

1. a reverse-apply/static absorption check against the current tree; and
2. a fresh run of the candidate's named acceptance files.

If both show absorption, skip the candidate before building. The first probe
returned `0` new files from three tasks, so historical-branch reuse is no longer
the active allocation strategy.

A second probe showed that the old unreached inventory was also stale: its four
fs one-offs and six HTTP/2 error-code files were already `4 / 4` and `6 / 6`
green. Do not dispatch directly from `compat/data/unreached-inventory.json`
without a current red run.

The first worklists generated from the latest full `gate-node` logs produced
three gains with no guard regression:

- worker `84 → 85 / 141` (timeout unchanged at 9);
- net `108 → 110 / 150` (timeout unchanged at 4);
- dgram `71 → 71 / 76` (timeout unchanged at 1);
- `test-runner-*` stayed `28 / 77`, although `test-runner-cli.js` advanced
  through two independent assertion layers.

**Current dispatch strategy:** regenerate disjoint eight-file worklists from the
newest full-run logs, prefer files with CLASS/CAUSE signatures, and iterate only
while a file advances. Agents run their named files and at most two direct
guards; integration owns the shared build, full related-subtree regression,
checkpoint, push, and PR update. Reuse the now-warmed worktrees: fresh worktrees
spent several minutes installing source dependencies while holding the global
build lock, which serialized the whole wave without using CPU.

## State

- **Integration branch**: `r9/integration`, pushed to origin. PR **32** targets `rewrite_bun_in_mcpp`.
- **Integration worktree**: `.claude/worktrees/wt5` (git + docs + measurement).
  `wt3` is a spare build/measure worktree.
- **Last authoritative node measurement**: `2566 / 4433` = 57.9% strict, 66.1%
  excluding self-skips, at commit `9d70bbf`. Run dir:
  `.claude/worktrees/wt5/target/integration/r12b`.

## FIRST TASK ON RESUME — four items, in this order

-1. ~~bun -256~~ **FIXED and re-measured: green back to 865** (868 before the
   round, 3 of the difference now counted as ahead-of-reference). Cause was one
   missing `writers: []` on the two `Bun.spawn` child records; every `Bun.spawn`
   threw. Two earlier diagnoses of mine were wrong and are recorded in the commit
   — both named real defects, neither was the cause.

0. **A 17x setImmediate performance regression, measured.** A 10 000-link
   setImmediate chain: **12ms before this round, 209ms after; real node does it
   in 10ms.** The semantics did not change — baseline, current build and node all
   print `A,B,A2` — so the cost bought nothing. Suspect the drain-batch stamp
   added to the Immediate phase in `process_web.cppm` this round. Guarded now by
   `latency_probe.py --only setImmediate` (400ms threshold; current build 565ms
   FAILS, pre-round baseline 114ms passes).

1. **The eleven "regressions" resolve into three groups. Two remain open.**

   **Fixed (5).** Four were ONE bug, and not in the child:
   `spawnSync`'s drain loop kept `(timeoutMs >= 0 && !timedOut)` as a disjunct,
   so once both pipes hit EOF it spun with zero fds until the deadline and then
   killed an already-exited child — a 25ms child cost the full 30s timeout
   (node 31ms, mbun now 92ms). That closed
   `test-uncaught-exception-handler-stack-overflow`, `-on-stack-overflow`,
   `test-async-hooks-stack-overflow-nested-async` and
   `test-runner-mock-timers-with-timeout`. The fifth,
   `test-permission-fs-require`, was the fatal reporter synthesizing
   `Name [CODE]` — no ordinary node error prints that.

   **Never regressions (3).** `test-web-locks`, `test-web-locks-query` and
   `test-worker-process-env` were FALSE PASSES at baseline. `navigator.locks` is
   undefined inside a worker on BOTH binaries; what changed is that worker errors
   now reach the parent instead of being swallowed, so the parent no longer exits
   0 over a failed worker assertion. Same class as the `assert.throws` fix that
   removed 126 unreal passes — the count went down because the measurement got
   honest. The underlying gaps are real work, but they are not new.

   **Still open (2).** `test-crypto-worker-thread` (a KeyObject appears to
   survive worker structured clone as a plain object dump) and
   `test-repl-tab-complete-nested-repls`. Untriaged.

   Method note that cost real time: for the four spawnSync files I probed the
   CHILD three times — a throwing `uncaughtException` handler, a stack overflow
   reaching the handler, a plain caught overflow — and all three came back
   *better* than baseline and matching node. The tests are about the PARENT.
   Read the failing file before probing the mechanism its name suggests.

2. **`w5/agent-http` is UNMERGED and worth +18.** It conflicts with the merged
   `w6/net-dgram` and `w6/child-cluster` work in `modules/jsc/src/js_net.cppm` —
   7 hunks, and both sides restructure the socket read path (HEAD has `onread`
   with a static buffer plus `_adopt(fd)`; the branch adds a read-side parking
   queue, `_flowing`, `_dataSink` and `_deliver`). It must be COMPOSED, not
   resolved by taking a side. Its headline fix is large: mbun's `net.Socket`
   silently dropped every byte that arrived before a `'data'` listener existed,
   which is why several "mustCall never fired" files were misattributed to http.
   Verify both feature sets behaviourally afterwards — a merge that compiles and
   has no conflict markers can still have lost one of them (that happened this
   round with the tick queue).
- **Last bun measurement**: green `868 / 1902`, at `8009cfc` — **stale**, predates
  waves 4-6. A fresh run is owed.
- **Frozen baseline binary for the current wave**:
  `target/baseline-w5/bin/mbun`. It MUST stay named `mbun` — a copy under any
  other basename breaks every test that re-spawns the runtime (`spawn mbun
  ENOENT`) and manufactures ~10 phantom regressions.

## How to resume

1. `bash tools/integration/build_or_die.sh` — it refuses on a toolchain mismatch
   and prints the real reason. The project needs **gcc 16.1.0**; the global
   `~/.mcpp/config.toml` default has been flipped to an unusable 15.1.0 twice by
   concurrent agents, and neither symptom (an ICE in `modules/ffi`, missing
   `std::byteswap` in `modules/crypto`) mentions a compiler version.
2. Check for unmerged agent branches: `git branch --list 'w*/*'` and diff each
   against the integration head. Agent worktrees live at `<repo>/wt1` … `<repo>/wt11`.
3. Merge, then run **both** `tools/integration/check_conflict_markers.sh` and
   `check_submodule_gitlinks.sh`. A merge has already silently replaced the
   `compat/{bun,node}` submodule gitlinks with worktree symlinks; it is invisible
   locally because the symlink resolves on the machine that made it.
4. Re-measure at integration and evaluate with
   `tools/integration/wave_report.py --before <old-run> --after <new-run>
   --corpus node --hours <wall-clock> --agents <n>`. It prints the throughput,
   the hours-to-100% at that rate, and which dispatch protocol the remaining
   shape calls for. **Do not choose the protocol by taste — it is computed.**
5. Cut the next lists with `tools/integration/make_worklists.py` and dispatch.

## Current protocol (long-tail phase)

Integration owns full-corpus measurement, cross-subsystem guards and build
verification. Agents own per-file repair plus a subsystem-subset run at
`--jobs 3`. Parallelism cap 10. Rationale and the resource discipline that makes
that safe are in `compat/README.md` under "Dispatch protocol".

## Honest target

100% on both corpora is **not reachable in 10 hours**: 2374 files remain, that
needs 237 files/hour, and the best measured rate is 28/hour with 3 agents (≈187
even assuming ten agents at double efficiency). Roughly 79% of the remaining work
is a defensible 10-hour target. Say so plainly rather than reporting progress
against an unreachable number.
