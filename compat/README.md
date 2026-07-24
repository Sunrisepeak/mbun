# Compatibility corpora

`compat/` is the single home for upstream compatibility inputs, runners, and
their recorded measurements.

## Layout

- `bun/` is the `oven-sh/bun` submodule, currently pinned to a `main` snapshot.
- `bun/test/` contains Bun's native test corpus.
- `compat/bun/bench/` contains Bun's native benchmark corpus.
- `node/` is the `nodejs/node` submodule, pinned to the selected Node.js release.
- `node/test/` contains Node's native test corpus.
- `data/` contains corpus inventories and benchmark result data.

Initialize the upstream repositories with:

```bash
git submodule update --init --recursive
```

The submodules are read-only inputs. mbun-specific adapters, fixtures, and
harnesses belong outside them.

## Bun corpus runner

**Prerequisite: the corpus needs its npm dependencies installed**, in *two*
places — `compat/bun` (the repo's own devDependencies) and `compat/bun/test`
(the corpus fixtures' packages):

```bash
(cd compat/bun && bun install --frozen-lockfile)
(cd compat/bun/test && bun install --frozen-lockfile)
```

Without them the numbers are badly wrong, not just slightly: **264 of the 1902
files** fail at file evaluation with `Cannot find module`, before a single
assertion runs — `esbuild` alone kills 76 files, because
`test/bundler/expectBundled.ts` imports it unconditionally at the top of the
shared bundler harness. Those files then look like one-assertion near-misses
when they are in fact dead. `node_modules/` is gitignored inside the submodule,
so a fresh clone always starts in this state. The runner refuses to start when
it sees it (`--allow-missing-node-modules` overrides, for a deliberate
measurement of the un-provisioned state).

```bash
MBUN="$(find target -type f -name mbun -perm -111 | head -n 1)"
python3 tools/integration/bun_corpus_runner.py \
  --bin "$MBUN" --cwd compat/bun \
  --discover compat/bun/test --sample-per-group 100000 \
  --out target/integration/bun-corpus --jobs 14 --timeout 30
```

`--cwd compat/bun` matters: bun's own CI runs from the bun repo root, so tests
that spawn relative fixture paths and the corpus `bunfig.toml` only work there.
`summary.json` reports per-file classifications; `green` counts files whose
every executed test passed and which reported no error outside a test.
Buckets that are explicitly **not** passes: `all-skipped` (every test skipped),
`no-tests` (the upstream file declares no runnable test), and
`blocked-external` (needs a service, registry, or toolchain this environment
does not have — the hand-triaged list is
`tools/integration/manifests/blocked-external.txt`).

## Node corpus runner

Node's upstream `test/parallel` files are plain scripts that expect exit
code 0 (no test runner). The runner executes each file directly through mbun;
harness services Node's own runner would provide (`// Flags:` comments,
internal bindings, env setup) are NOT emulated, so files that need them count
as failures. The result is honest file-level coverage, not an API checklist.

```bash
python3 tools/integration/node_corpus_runner.py \
  --bin "$MBUN" \
  --out target/integration/node-parallel --jobs 14 --timeout 15
```

Measurement data is stored under [`compat/data/`](data/), including test and
benchmark inventories and native run results.

## Estimating a round target

Targets used to be guesses. [`data/round-estimates.json`](data/round-estimates.json)
records estimate-vs-actual per task so they stop being guesses; round 9's five
tasks produced a clear and initially counter-intuitive signal.

**Cluster homogeneity predicts the hit rate. Cluster size does not.** The two
largest work-lists delivered 51% and **13%** of target; the three smallest
delivered 132–187%.

| work-list | homogeneity | target | actual | hit rate |
| --- | --- | --- | --- | --- |
| 611 files — harness flag re-spawn | low | +150 | +76 | 0.51 |
| 580 files — the timeout bucket | low | +120 | **+15** | **0.13** |
| 74 files — node:fs | high | +30 | +56 | 1.87 |
| 78 files — buffer/zlib/url/net | high | +25 | +33 | 1.32 |
| 70 files — crypto/webcrypto | high | +22 | +30 | 1.36 |

A *homogeneous* cluster is one where the files fail for the same reason at the
same layer — fix the layer, they all convert. A big cluster sharing only a
*symptom* (one log line, one bucket) is not one root cause: unblocking it
exposes each file's own unrelated second failure. Both large round-9 clusters
were symptom-clusters and behaved accordingly.

Rules this produces, in order of how much they cost when ignored:

1. **Verify the cause before setting a target on it.** The 580-file timeout
   target assumed handle ref-counting. The actual cause was every event-pump
   phase swallowing thrown exceptions. Work on an unverified causal hypothesis
   is exploration; give it an exploration budget, not a conversion target.
2. **Score the probe the way the target is scored.** The 611-file target came
   from a probe counting self-skips as conversions while the target counted only
   real passes — a bias baked in before the work started.
3. **Re-measure the baseline with the binary under test.** A work-list built
   from an older run silently contained 38 already-green files out of 78.
4. **For a symptom-cluster, target the diagnosis, not the pass count.** Moving
   429 files from a 15-second hang to a sub-second diagnosable failure made the
   corpus triageable and was worth doing — it is just not a pass count, and
   reporting it as one would be dishonest.

**Wall-clock:** one task runs 57–119 minutes (median 83). A coordination tick
shorter than that cannot be a round boundary — it is a checkpoint. Plan a round
as *dispatch → several checkpoints → integrate*, never as one tick.
