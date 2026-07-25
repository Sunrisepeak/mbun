# RESUME — where the corpus push stands

Written by the integration agent after every wave and before every dispatch, so a
session that is interrupted (usage limit, crash, restart) can pick up from the
file rather than from memory. **If you are a fresh session reading this, start
here.**

Session-scoped cron jobs are in-memory only (`durable` has no effect), so the
hourly loop survives a usage limit — it simply skips the fires that land during
the block and resumes within an hour — but it does NOT survive the session
ending. This file is what makes that recoverable.

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

1. **Eleven real regressions from this round's runtime-wide changes.** Each
   reproduces 3/3 standalone, so they are not guard artefacts. Four time out:
   `test-async-hooks-stack-overflow-nested-async`,
   `test-uncaught-exception-handler-stack-overflow`,
   `-on-stack-overflow`, `test-runner-mock-timers-with-timeout`. Seven fail:
   `test-web-locks`, `test-web-locks-query`, `test-crypto-worker-thread`,
   `test-permission-fs-require`, `test-repl-tab-complete-nested-repls`,
   `test-worker-process-env` (this last one the worker agent identified as a
   pre-existing latent failure that the new error reporting stopped hiding).
   Suspect the `process.nextTick` queue split (`engine.inc`, `__mbunRunTicks`)
   and the entry-script try/catch, both landed this round.

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
