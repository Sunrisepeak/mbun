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

## Security audit findings (round 10)

A dedicated audit repeated one question across 15 surfaces — *what does this code
CLAIM to enforce, and what does it actually do?* — and found **six defects
sharing that shape: a control that reports success while not being applied**.
**None of them moved a compatibility count**, which is why ordinary corpus work
would never have surfaced them.

| # | Finding | Severity |
| --- | --- | --- |
| 1 | **`Bun.$` was a complete `--permission` sandbox escape** — `Bun.$\`cat /etc/passwd\`` read files, `>` wrote them, and re-exec dropped the sandbox entirely, all while `fs.readFileSync`/`execSync`/`Bun.spawn` were correctly denied | **critical** |
| 2 | **ChaCha20-Poly1305 accepted a zero-length auth tag** and returned tampered plaintext. Two defects composed: the JS layer read `mode` as `"stream"` so `setAuthTag` skipped its length check, and the native layer skipped `EVP_CTRL_AEAD_SET_TAG` for an empty tag, leaving OpenSSL's taglen at 0 — and `CRYPTO_memcmp(a, b, 0)` compares equal | high |
| 3 | **`Bun.serve` HTTP request smuggling** — `Content-Length` + `Transfer-Encoding` produced *two* handler invocations on one connection | high |
| 4 | **Response splitting via `Response.statusText`** — it reached the wire verbatim | high |
| 5 | **`bun:sqlite` bypassed the fs gates**, including `ATTACH DATABASE` opening a second file from inside a SQL string where a path gate cannot see it | medium-high |
| 6 | **`Bun.Glob` bypassed the fs read gate** | medium |
| 7 | **`zlib` `maxOutputLength` did nothing on streams** — the documented defence for untrusted input, unenforced exactly where untrusted input arrives | medium |
| 8 | **CSPRNG failed open** to `Math.random()` if the native binding were absent | low |

Nine surfaces were checked and found **sound**, with their probes kept so the
next audit does not redo them: `timingSafeEqual` (measured constant-time),
`checkServerIdentity` (25/25 RFC 6125 cases), `rejectUnauthorized` (15 live
bypass attempts against an untrusted CA, all rejected), `vm` realm isolation (17
escape attempts), `execFile`/`spawn` shell avoidance, path-NUL rejection across
19 fs APIs, key-class confusion, `structuredClone` detach, and the CSPRNG engine
itself.

**Two things the audit got right that are worth imitating.** It used bun's own
`request-smuggling.test.ts` as an independent oracle, and that suite caught its
first fix being *over-strict* — RFC 9112 §6.3 permits duplicate `Content-Length`
with identical values, and multiple TE field-lines that combine to a valid list.
It corrected rather than banking the stricter version. And its first before/after
used a pre-existing binary from a different `target/<fingerprint>/` and reported
**64 phantom regressions**; a stale-fingerprint binary is not a baseline.

Left open with reasons: WHATWG IPv4 literal parsing (`0177.0.0.1` →
`177.0.0.1` where node gives `127.0.0.1`, while `dns.lookup` resolves it to
loopback — SSRF-relevant, but no end-to-end connect was demonstrated), UTS46
mapping of U+3002, `aes-*-ccm` being non-functional (fails closed and loudly, so
a gap rather than a hole), and `tls.rootCertificates` carrying one entry against
node's ~150.

## Two corpora, one contract

mbun is measured against **two** upstream corpora: bun's and node's. That
invites a wrong conclusion — that advancing one must cost the other, and that
someone has to keep choosing. **On the evidence so far, that trade is almost
entirely an artefact of measurement, not a real conflict of contracts.**

The case that exposed it was `assert.deepStrictEqual`. mbun deliberately
implemented **bun's** semantics; a comment in
`modules/jsc/src/builtins/node_assert_deepequal.cppm` stated that "bun's
contract — not vanilla node — is the blueprint". But the bun file it was pinned
to, `compat/bun/test/js/node/assert/deep-equal.test.ts`, says in its own header:

> Expectations come from the documented semantics of
> `assert.deepStrictEqual`/`deepEqual`, **cross-checked against Node.js**. Cases
> **Bun gets wrong today** are marked `test.failing`.

and its case type has `strictBug?: string` — *"Set when **Bun disagrees** with
`strict`; the text says **what Bun does instead**."*

So bun's corpus encodes **node's** semantics and labels bun's own deviations as
bugs. There was never a conflict to arbitrate. mbun had implemented bun's known
bugs on purpose, because of how the corpus scored them.

### Why it looked like a conflict: the runner scored correctness as regression

`mbun test` implements `test.failing` and reports a marker that starts passing as

```
(fail) a known bug that is now FIXED — expected to fail but passed
 2 pass
 1 fail
```

and `bun_corpus_runner.py` mapped any `failed > 0` to `test-failure`. **Being
more correct than the reference implementation was scored as a compatibility
failure**, so every step toward node looked like a step away from bun. That is
what manufactured the "which corpus do we serve?" dilemma.

Fixed: a file whose only failures are stale `test.failing` markers is now
classified **`ahead-of-reference`** — its own bucket, neither a pass nor a
failure. A real failure alongside a stale marker is still `test-failure`, and a
non-zero exit still dominates.

### The rule this gives us

**Maintainer decision (recorded):** where the divergence is a **bun bug**, mbun
fixes it rather than reproducing it. Compatibility means matching the documented
contract, not replicating a defect. If fixing it also fixes bun's own test, so
much the better — that has already happened: the round-10 security audit's
request-smuggling fix took **bun's own** `request-smuggling.test.ts` from 53 to
61 passing, the 8 gained being exactly its security assertions.

1. **Node's documented semantics are the blueprint for any `node:*` API**, on
   both corpora. Where bun differs and marks it a bug, mbun follows node.
2. **A genuine conflict is one where bun deviates and does *not* call it a
   bug.** Those exist in principle and must be escalated to the maintainer with
   both contracts and the file counts on each side — never resolved silently by
   an agent.
3. **Never let a measurement artefact define a policy.** A corpus is evidence
   about a contract, not the contract itself. Read the upstream test's own
   statement of intent before treating its expectations as a requirement.

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
