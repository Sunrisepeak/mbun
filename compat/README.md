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

**Wall-clock:** one task runs 57–119 minutes (median 90). A coordination tick
shorter than that cannot be a round boundary — it is a checkpoint. Plan a round
as *dispatch → several checkpoints → integrate*, never as one tick.

### Signature clustering is blind to a missing layer (round 10)

The `node:http` list was 295 files that `cluster_finder` showed as a
heterogeneous tail — 76 of them under a bare `AssertionError` with no shared
text. Target +60. **Actual +170 (2.8×), `test-http-*` 102 → 272 green.**

Those 76 were not 76 causes. They were mostly *one*: `node:http` was a
shape-only stand-in, with a second, incompatible `ServerResponse` living in the
transport layer. Translating node's own five `_http_*.js` files instead of
extending the stand-in converted **148 files in a single change**.

**Signature clustering cannot see this.** Every file asserts something different
about the missing layer, so the logs share no signature and the cluster looks
like scattered work. Two checks catch it where the signatures cannot:

1. **Green ratio.** For a mature, stable node subsystem, a low green ratio is
   stronger evidence of a missing layer than the error texts are. `http` was
   102/398 = 26% on an API that has been stable for a decade — that fact alone
   was the signal, and it was sitting in the report the whole time.
2. **Stand-in or translation?** Check whether mbun's implementation is a port of
   node's `lib/`, or hand-written to shape. A stand-in producing many small,
   unrelated assertion failures means *translate the layer* — do not extend the
   stand-in.

By that heuristic, `cluster` (11/80 green) and `runner` (10/73) were picked as
the next target — and it held. `node:cluster` was a **stub whose `fork()`
returned a bare EventEmitter**; translating node's `lib/cluster.js` +
`lib/internal/cluster/*` was +38 by itself, and the subsystem went **11/83 →
76/83 green**. Signature clustering had shown those 45 files as a bare
`global code@…` bucket — "a top-level throw" — which is no signal at all.

**The green ratio is therefore the primary targeting input; signatures are for
picking work *within* a subsystem.** Two independent confirmations in one round:
`http` 26% green → stand-in → +170 (2.8× target); `cluster` 13.3% green → stub →
+73 (1.62×).

The same task also fixed two transport bugs the port exposed, both of which had
been silently corrupting correct-looking output: `net.Socket._flush()` stopped
at the first zero-byte write, so a single empty chunk parked every byte behind
it forever — which **silently lost the second response on every keep-alive
connection** — and pipelined intake dropped bytes because `push()` runs the
request handler synchronously and the parser could be replaced mid-loop, so a
10,000-request pipeline stalled dead at exactly 1,771.

### Iteration count matters as much as cluster shape (round 10)

Round 9 concluded that homogeneity predicts the hit rate. Round 10 partly
overturns that: a **400-file, medium-homogeneity** http2/tls list delivered
**1.50×** its target — a shape round 9 would have predicted to under-deliver.

The difference was method, not the cluster. That task ran **eight
build→measure cycles**, re-clustering the remaining failures after every one and
holding a zero-regression gate at each, instead of making one large change and
measuring at the end. Each cycle retargeted the next-densest surviving cause, so
the work-list effectively became homogeneous *during* the round rather than
having to be homogeneous at dispatch. It stopped when re-clustering showed no
remaining cause above 5 files — an evidence-based stop, not a budget one.

So the rule is now two-part:

- Homogeneity at dispatch predicts the hit rate **for a single-pass task**.
- An **iterative** task can manufacture homogeneity, so a large heterogeneous
  cluster is worth attempting *if* the agent re-clusters between cycles. Ask for
  that explicitly; it is not the default behaviour.

Corollary for briefs: naming a *suspected* root cause does not bias a good
agent. Round 10's repl brief named a suspect that was directionally right and
wrong in detail (it blamed "something" replacing the global timers; the culprit
was `node:domain` doing so on first `require`, to stand in for missing
`async_hooks`). The agent re-derived the real cause and hit its target exactly.

Also: **do not record an agent's self-reported elapsed time.** One task reported
"~3h15m" for work that took 77–80 minutes by both wall clock and the harness
timer. Use the measured value.

### Make the wall name itself (round 10)

The strongest single technique observed so far, for a cluster whose depth is
unknown: **stub the missing capability so it fails loudly with its own name**,
then let the log histogram choose the next target each cycle.

`internalBinding` was stubbed to throw `No such binding: <name>`. Each cycle's
logs then named the next densest wall exactly, with no guessing:

| cycle | added | pass |
| --- | --- | --- |
| baseline | — | 0 |
| 1 | `primordials` + the throwing stub | 8 |
| 2 | `util`, `constants`, `uv`, `errors`, `options`, … | 21 |
| 3 | `string_decoder` (94 files were gated on `internal/util` reading `.encodings`) | 35 |
| 4–7 | `buffer`, `os`, `messaging`, `builtins`, `module_wrap`, `worker`, `async_wrap`, … | **45** |

Per-cycle deltas were +8, +13, +14, +4, +3, +1, +2. **A single blind change
would have reached ~21 of the 45.** The technique converts an open-ended
"implement `internalBinding`" into a measured, self-terminating work queue, and
the tail (+1, +2) is the signal to stop.

It also settled a question this project had deferred **twice**: `primordials`
alone converts 8/305 = **2.6%** (reproducing an earlier 2/60 probe at 5× the
sample), while `primordials` + `internalBinding` converts 45/305 = **14.8%**.
`internalBinding` does roughly 90% of the work. Deferring on the primordials-only
number had been correct; the missing measurement was the combined one.

### Before optimising a metric, audit the metric

Round 10 found **three independent defects in how the corpus was verified**, each
of which had been inflating the number for an unknown length of time:

1. **Self-skips counted as passes.** `common.skip()` exits 0; 277 files of one run
   were skips reported as coverage.
2. **`common.mustCall` was never enforced**, because node registers its verifier
   inside `process.on('exit')` and that never fired. 946 of the then-1,533
   passing files used `mustCall*`.
3. **`assert.throws` ignored its error argument.**
   `assert.throws(fn, {code:'ERR_X'})` passed for *any* throw, and
   `assert.throws(fn, common.expectsError({…}))` never called the validator. 48
   verified vacuous passes on the guard sets alone.

All three were found by agents doing unrelated work who stopped to ask *why* a
test passed. None would have been found by pushing the number up.

**A verification-mechanism fix must be sequenced FIRST in a round.** It
invalidates every other task's baseline corpus-wide, not just inside its own
work-list. This was recorded after defect 2 and then violated with defect 3 — two
agents were already running on a pre-fix base when it landed, so their reported
numbers include vacuous passes and had to be re-measured after integration. When
a task's diff touches `assert`, `common`, the exit path, or the classifier, land
it alone and re-baseline before dispatching anything else.

### Derive the guard set from the diff, not from the brief

A zero-regression gate is only as wide as the files it runs. Round 10 lost three
`test-dns-*` files to a task whose guard covered `http`/`https`/`tls`/`net`/`dgram`
— its targets — while the change also edited `dns`. The gate passed; the
regression was found two merges later by a full run.

**Rule:** after the work is done, run `git diff --name-only`, map the changed
builtins/modules to every corpus subsystem that loads them, and guard all of
them. The brief names where you are *aiming*; the diff names what you can
*break*.

### Isolated deltas are not additive

Round 9's eight tasks each measured a gain against their own cluster, each with
a zero-regression gate. They summed to **+339**. The composed branch measured
**−232**.

Nothing was faked and no task was wrong. One task made `process.on('exit')`
fire, which is where node registers `common.mustCall`'s verifier, and stopped
the event pump swallowing thrown exceptions. That turned a *global verification
mechanism* on. 496 files went green→non-green; all 496 were classified from
their logs and **495 had been passing without verifying anything** — 275
assertions that now actually run, 176 `mustCall`/`mustNotCall` violations, 19
verified vacuous passes, 26 whose round-8 logs were empty (the signature of a
swallowed throw). Exactly one was a real regression.

So:

- **Per-cluster deltas only add up while every task is a local fix.** The moment
  one task changes how the corpus is *verified*, every other task's baseline is
  invalidated corpus-wide, not just inside its own work-list.
- **Sequence such a task first, or measure it last.** Landing the verification
  change before the others would have given every other task an honest baseline
  to work against, at the cost of making their targets look smaller.
- **A round that lowers the headline can still be the most valuable one.**
  Timeouts fell 583 → 169 and the number finally means what it says. Report the
  drop and its classification; do not report the sum of the parts.
- **Re-rank clusters against the composed binary, never the pre-round one.**
  Doing so immediately surfaced a self-inflicted bug that had become the corpus's
  largest single cluster (265 files across 79 subsystems: two internal globals
  left enumerable, which node's `common` reports as leaked — and which fired
  *before* ~300 files could reach `common.skip()`, so they reported `fail`
  instead of `skipped`).
