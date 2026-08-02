# W43 Node/Bun corpus sprint ledger

Date: 2026-08-02

Tracking issue: #80

Target branch: `rewrite_bun_in_mcpp`

Campaign branch: `agent/corpus-coverage-w43`

## Current status

Wave A is integrated at `d62163c`: its five logical lanes produced 13 focused
new-green files with no focused or impact-list classification regression. Node
contributed +7 and Bun +6. A1 reached +4, A2 was deliberately narrowed to a
safe +1 below its +2 floor, A3 reached +2, A4 reached +3 after two additive
independent-review fixes, and A5 reached +3 through two separately attributed
mechanisms. This is not a post-Wave-A full-corpus result; checkpoint 0 remains
the authoritative full baseline until the final same-binary full run.

Five logical lanes will be executed in rolling batches because this session has
three physical worker slots. The coordinator alone owns builds, full corpus
runs, integration, metrics, and GitHub milestone comments.

## Frozen provenance

| Field | Value |
| --- | --- |
| Target base | `163cb6d3fedb9d22cf069cb5f8e20fcb2bc76049` |
| Baseline source head | `0cfcd5915947c0f709e482165e96aef9531bc337` |
| Binary location | Git common directory `w43/baseline/mbun` |
| Binary SHA-256 | `5e17080bb82a6c5ac46c56c8c66070409371e46e368bdf285304f42695f8d395` |
| Runtime versions | mbun `2026.07.18.0`; Bun `1.3.14` compatible; Node `v26.3.0` compatible |
| Node window | 2026-08-02 15:34:56–15:50:14 +08:00; 917.4 s |
| Bun window | 2026-08-02 15:50:29–16:08:34 +08:00; 1085.4 s |

The first build attempt was correctly rejected at link time. Root-cause evidence
showed that the worktree-local installed JSC package contained the GCC 15.1
`libstdc++.a` byte-for-byte while the pinned compiler was GCC 16.1. The local,
generated dependency copy was refreshed from the recipe-required GCC 16.1
archive; no repository source or corpus input changed. `build_or_die.sh` then
linked successfully and the frozen binary above was created. This provisioning
recovery is not counted as compatibility progress.

## Reproduction commands

```bash
W43_COMMON_DIR=$(cd "$(git rev-parse --git-common-dir)" && pwd)
W43_BASE_BIN="$W43_COMMON_DIR/w43/baseline/mbun"

python3 tools/integration/node_corpus_runner.py \
  --bin "$W43_BASE_BIN" \
  --root "$PWD" \
  --out target/integration/w43-node-baseline \
  --jobs 4 \
  --timeout 15

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

Both category sums equal their raw denominators. The Bun run used `memory_max =
4G` and `tasks_max = 512`.

## Checkpoint 0 full baseline

### Node `test/parallel`

| Classification | Files |
| --- | ---: |
| pass | 3,138 |
| fail | 660 |
| skipped | 535 |
| timeout | 97 |
| oom-kill | 3 |
| total | 4,433 |

- Raw pass rate: `3,138 / 4,433 = 70.79%`.
- Audited runnable floor: `3,138 / 3,898 = 80.50%`.
- Runnable gap: 760 files.

### Bun `test/**`

| Classification | Files |
| --- | ---: |
| green | 1,041 |
| test-failure | 704 |
| all-skipped | 73 |
| timeout | 44 |
| blocked-external | 19 |
| no-tests | 6 |
| ahead-of-reference | 4 |
| load-error | 4 |
| oom-kill | 4 |
| crash | 3 |
| total | 1,902 |

Test-level counters: 33,861 passed, 13,452 failed, 50,163 ran, and 467,237
expects.

- Raw green rate: `1,041 / 1,902 = 54.73%`.
- Audited runnable floor: `1,041 / 1,804 = 57.71%`.
- Runnable gap: 763 files.

### Combined

- Raw: `4,179 / 6,335 = 65.97%`.
- Audited runnable floor: `4,179 / 5,702 = 73.29%`.
- Runnable gap: 1,523 files.
- Exclusions are not passes. The 3,898 and 1,804 denominators are floors and
  may increase when capabilities are enabled; they may not shrink through new
  skips or exclusions.

## Lane ledger

| Lane | Corpus/owner | Fixed target | Actual | Issue | Commit | State |
| --- | --- | ---: | ---: | --- | --- | --- |
| A1 | Node zlib/Buffer | +3 to +5 | +4 | #83 | `71b2c2b..bacd246` | accepted and composed |
| A2 | Node assert | +2 to +4 | +1 | #82 | `e7b8d99..0a102f6` | accepted safe partial; target missed |
| A3 | Node permission | +2 to +4 | +2 | #84 | `c48839f..bef02c6` | accepted and composed |
| A4 | Bun N-API | +3 to +5 | +3 | #86 | `29e12e3..e9207a3` | accepted and composed after two additive review fixes |
| A5 | Bun test runner | +3 to +5 | +3 | #87, #88 | `d222075..444e495` | accepted and composed |

All 49 literal paths were found in the fresh baseline and were non-green. A1,
A2, and A3 contain respectively 5, 8, and 10 Node `fail` rows. A4 contains 18
Bun `test-failure` rows and A5 contains 8; every Bun row has exactly one failed
test at baseline. The five manifests are path-disjoint.

Retired-approach gate:

- A1 zlib/Buffer and A3 permission have no matching struck record.
- A2's broad `assert` query returns existing records in unrelated process,
  crypto, HTTP/2, and TLS areas; no listed A2 path or proposed assert-deep-equal
  mechanism is named as retired.
- A4 must not repeat the old claim that all N-API addons are blocked, nor repeat
  the already-landed dynamic-symbol/global fix. That verdict is overturned;
  the remaining files require real per-entry-point N-API diagnosis, while five
  shared-libstdc++ addon cases remain outside a runtime-only quick fix.
- A5 must not blindly re-land the previously reverted global bunfig preload
  behavior: it can delete the runner's private TMPDIR and has whole-corpus blast
  radius. Any preload-related result must preserve sandbox environment state and
  remains blocked on the final full Bun gate.

Worker probes, focused results, integration deltas, serial confirmations, and
remaining reds are appended here only after coordinator review. Full-corpus
numbers are updated only at the next same-binary full checkpoint.

## Wave A batch 1 composed checkpoint

| Evidence | Result |
| --- | --- |
| Coordinator head | `7e84e13efc1da8599aacd31daf28eb8603597c0f` |
| Frozen composed binary | Git common directory `w43/wave-a/mbun` |
| Binary SHA-256 | `4efb3448ebd7b2e34f0eda1e15dff74201aaae25ed7ce9a01ca607f21ca4b0b6` |
| Fresh build | `build_or_die.sh --no-cache`: pass |
| JSC member suite | 29 passed, 0 failed |
| Permission member suite | 1 passed, 0 failed; 150 checks, 0 failures |
| A1 manifest | 0/5 -> 4/5; four serial repeats passed |
| A2 manifest | 0/8 -> 1/8; one serial repeat passed |
| A3 manifest | 0/10 -> 2/10; two serial repeats passed from the pinned Node cwd |
| Focused total | 0/23 -> 7/23; no green-to-non-green move |
| Node impact run | 653 files; 421 pass, 149 fail, 73 skipped, 9 timeout, 1 OOM; diff gate found no regression |
| Bun impact run | 180 files; 100 green, 59 test-failure, 8 all-skipped, 7 timeout, 3 ahead-of-reference, 2 blocked-external, 1 OOM; diff gate found no regression |

The first full JSC run exposed `test_async_hooks` intermittently missing only
the `timeout:value` observation. Ten-run isolation measured the pre-existing
one-millisecond deadline race across A1, A2, A3, and the composed tree. Issue
#85 replaced the one-shot due-only drain with the runtime's bounded event-loop
pump; the exact test then passed 10/10 and the full JSC suite passed 29/29. No
runtime assertion or product behavior was weakened.

A1's only remaining manifest red is the independent `DEP0005` warning-delivery
case. A2 retains seven reds because review removed an incomplete handwritten
partial-deep comparator and did not expose unsupported `skipPrototype`
semantics. A3 retains eight separately diagnosed permission surfaces. These
remaining rows are not counted as gains or waived.

## Wave A final composed checkpoint

| Evidence | Result |
| --- | --- |
| Coordinator product head | `d62163c92df55dc6126caf31f564dd2ebad1d5e5` |
| Frozen composed binary | Git common directory `w43/wave-b-base/mbun` |
| Binary SHA-256 | `445b40f7eae331c05fc5fbbd92f8cb816b37979c7b3339545b02a1525663bb0a` |
| Fresh coordinator build | `build_or_die.sh`: pass, key `5c54b97c0110b62e` |
| Version probe | mbun `2026.07.18.0`; Bun `1.3.14`; Node `v26.3.0` |
| Full JSC member gate | 29 passed, 0 failed |
| Five-manifest focused gate | 0/49 -> 13/49; Node +7, Bun +6 |
| Serial deciding gate | all five manifests repeated at jobs 1 with the same 13 greens |
| Node impact gate | 415 files; 273 pass, 87 fail, 44 skipped, 11 timeout before and after; no regression |
| Bun impact gate | 34 files; 14 green, 16 test-failure, 1 all-skipped, 1 blocked-external, 1 load-error, 1 timeout before and after; no regression |

Wave A's fixed target was +12 to +20, so the measured +13 meets the wave target.
The original five-hour sprint target remains +30 to +45 and is not met by Wave
A alone. No timeout, OOM, crash, or exclusion classification moved in the 49
focused rows.

The composed before-to-after lane results are exact and same-binary:

- A1: 0/5 -> 4/5, leaving the independent `DEP0005` warning-delivery row red.
- A2: 0/8 -> 1/8, retaining seven deliberately unwaived assert rows.
- A3: 0/10 -> 2/10, retaining eight separately diagnosed permission rows.
- A4: 0/18 -> 3/18, leaving 15 N-API/libuv rows as `test-failure`.
- A5: 0/8 -> 3/8, leaving five reporter/preload/parser rows as
  `test-failure` and outside the accepted mechanisms.

A4's first independent review found that a missing or throwing configurable
`__mbun_uncaught` dispatcher could consume a finalizer exception silently.
Additive commit `d9fdc85` arms the shared fatal channel directly: missing or
non-callable dispatch preserves the original error/status 1, while lookup or
handler failure preserves the nested error/status 7. Scoped rereview then found
that the standard dispatcher's false fatal result was ignored. Additive commit
`e9207a3` consumes that result and stops the remaining finalizer batch. Real
probes exit 1 and 7 respectively; a two-finalizer behavioral member test proves
the second callback does not run after fatal. Final rereview reported no
Critical or Important findings.

A5 commit `d222075` keeps callback and returned-Promise completion independent,
so synchronous `done()` cannot hide a later rejection (+1). Commit `444e495`
bridges a lexical symlink path to the already-loaded canonical module-cache key
without evaluating an unloaded module, then uses the existing subscription
fan-out (+2). Both independent reviews reported no Critical or Important
findings. The canonical retry is proved on POSIX; Windows drive-letter/junction
identity remains unproved and is not claimed.

The name-derived impact gate is intentionally not a full regression proof. It
cannot infer behavior-only reachability such as GC ordering or settlement
timing; the fixed manifests, serial repeats, native behavior tests, and final
full corpus run cover those boundaries. On the Bun impact list, classifications
were unchanged while `js/node/fs/fs.test.ts` improved by 11 passed tests; this
is a test-level move, not an additional green-file credit.
