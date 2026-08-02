# W43 Node/Bun corpus sprint ledger

Date: 2026-08-02

Tracking issue: #80

Target branch: `rewrite_bun_in_mcpp`

Campaign branch: `agent/corpus-coverage-w43`

## Current status

Checkpoint 0 is the pre-implementation baseline. No W43 product source change
has started. The first five-hour sprint target remains net +30 to +45 fully
green files; the final campaign target remains the complete audited runnable
denominator.

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
| A1 | Node zlib/Buffer | +3 to +5 | — | — | — | not started |
| A2 | Node assert | +2 to +4 | — | — | — | not started |
| A3 | Node permission | +2 to +4 | — | — | — | not started |
| A4 | Bun N-API | +3 to +5 | — | — | — | not started |
| A5 | Bun test runner | +3 to +5 | — | — | — | not started |

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
