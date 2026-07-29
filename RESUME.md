# RESUME — where the corpus push stands

Written by the integration agent after every wave and before every dispatch, so a
session that is interrupted (usage limit, crash, restart) can pick up from the
file rather than from memory. **If you are a fresh session reading this, start
here.**

## 2026-07-30 10:50 — WAVE 55: `process.nextTick` runs INSIDE the microtask queue

The highest-leverage finding of the campaign so far, and it was found by chasing
one hanging file. **mbun violates node's core ordering contract: microtasks must
fully drain before the tick queue runs.** Fifteen-line repro, verified on the
current build by the integration agent (not just by the lane):

```js
Promise.resolve()
  .then(() => { log('mt1'); process.nextTick(log, 'TICK'); })
  .then(() => log('mt2'));
// node: mt1 mt2 TICK      mbun: mt1 TICK mt2
```

Cause: `process.nextTick` arms its drain with a **promise reaction** at
`modules/jsc/src/runtime/bindings_install.inc:411`. A tick scheduled from inside
a microtask therefore interleaves into the middle of that microtask chain
instead of waiting for it to empty.

**Exposure pattern — search for this shape, not for `nextTick`:** *a
cleanup/teardown step scheduled on `nextTick` that races an error travelling
through a promise chain.* Known-affected surfaces: stream teardown
(`destroyImpl`, `end-of-stream`), `pipeline`, `finished()`, async-iterator
adapters, and anything using `once()`-style listener removal.

**How it manifested.** `pipeline()` removes the tail's `'error'` listener once it
completes without error (`lastStreamCleanup`, nodejs/node#35452). Node is safe
because the tail is destroyed *before* pipeline completes — the microtask queue
drains first. Under the inversion, `'finish'` lands first, pipeline declares
success, drops the error listener, and a body throw arriving afterwards has no
listener: uncaught, and the composed stream hangs forever. So this was **never a
`compose`/`fromAsyncGen`/`nextAsync` gap** — those are all correct; `compose` is
merely the most ordering-sensitive consumer.

**What shipped (`c87e3af`) is a compensation, NOT the cure.** 24 lines in
`internal/streams/compose` (`node_stream_pipeline.cppm`) tracking
`pipelineFinished` plus a tail `'error'` listener routing a post-completion
error into the existing `d.destroy(err)` seam, gated so pipeline's own `onError`
stays sole owner while running.

Measured (integration agent, independent of the lane, vs
`codex-sprint2-wave39-full-node`): `test-stream` **236 → 237 / 249**, `test-http`
628/734 unchanged, `test-webstream` 10/16 unchanged, **0 regressions in all
three**. NORMAL-RETURN CHECK passed: `Readable.from([1,2,3]).compose(...)`
resolves `[2,4,6]` with exactly one `'end'`, one `'close'`, zero `'error'`.

**The cure needs its own lane, and it is expensive.** Correct fix: stop arming
the tick drain with a promise reaction; drain ticks only at the C++
microtask-drain boundary (`__mbunRunTicks` after JSC's own drain). Deliberately
NOT attempted here — the in-code history at `bindings_install.inc:401` records
that splitting `nextTick` out of `queueMicrotask` once cost **256 bun files**.
Any attempt must gate on the full bun corpus *and* node async/stream/http
subtrees before it is allowed to survive. Treat a "+N node files" result on that
lane as meaningless until the bun number is in hand.

Also reclassified this wave: `test-stream-iter-readable-interop.js` →
**known-blocked**, not a candidate.

### Protocol: how to run a full corpus measurement without it fighting the build

Two rules, both learned the hard way, and together they make the headline number
obtainable again after a long run of "full runs keep dying":

1. **`--resume`.** A bare full run dies at session/turn boundaries and leaves
   nothing. With `--resume` the partial results survive and the next invocation
   picks up where it stopped, so the run is no longer all-or-nothing.
2. **Measure against a FROZEN COPY of the binary, not the live build output.**
   Copy it out of `target/<arch>/<hash>/bin` first:
   ```
   cp "$(bash tools/integration/build_or_die.sh | tail -1)" target/integration/frozen-bin/mbun-<tag>
   python3 tools/integration/node_corpus_runner.py --bin target/integration/frozen-bin/mbun-<tag> --jobs 3 --resume --out ...
   ```
   `node_corpus_runner.py` deliberately exempts binaries living outside
   `target/<arch>/<hash>/bin` from its staleness refusal, precisely so frozen
   baselines can be old. Without this, integrating a lane rebuilds the tree, the
   live binary's hash directory changes underneath the running measurement, and
   the run is silently measuring two different builds — the exact stale-binary
   class the runner now refuses. With it, integration builds and a long
   measurement can proceed at the same time.

Corollary: **a full-corpus number belongs to a frozen tree.** Do not derive a new
headline total by summing subtree deltas onto an old total — that is arithmetic
presented as measurement. Report subtree deltas as subtree deltas until a real
full run lands.

## 2026-07-30 09:30 — WAVE 54: `Buffer.toString('utf8')` was silently eating BOMs

**`test-stream` 234 → 236/249**, and the more important find is nowhere near streams.

### A silent data-corruption bug in a very hot path

`rawUtf8Slice` (`node_buffer_extra.cppm:271`) decoded through
`new TextDecoder("utf-8")`, and the WHATWG default **strips a leading U+FEFF**. So
`Buffer.prototype.toString('utf8')` — and therefore
`fs.readFileSync(file, 'utf8')` — silently dropped byte-order marks. Fixed with
`{ ignoreBOM: true }` through a lazily-created singleton decoder.

Verified: `Buffer.from([0xEF,0xBB,0xBF,0x68,0x69]).toString('utf8')` now keeps the
BOM. Guarded over **1,409 node files** (`stream`/`buffer`/`fs`/`http`/`webstream`)
and 38 green bun buffer/encoding/fs files: **+15 gains, 0 regressions** on the node
side, 38/38 still green on bun.

This is the same class as the earlier typed-array `byteOffset` bridge bug: a
platform default quietly differing from node's, in a path nothing thinks to test
directly.

### `Readable.prototype.pause`/`resume` on a destroyed stream

`r.destroy(); r.resume(); r.pause()` emitted `'pause'` synchronously and scheduled a
`'resume'`. Node 26 (nodejs/node#62557) makes both no-ops once destroyed. Landed with
`resume` guarded on `kDestroyed && state.length === 0` — **deliberately narrower than
node's**, mirroring why bun narrowed it (`internal/streams/readable.ts:1136-1146`):
fd-slicer-style readables set `destroyed` immediately before `push(null)`, and a full
guard strands the buffered tail.

### Two stream failures are NOT stream bugs

- **`test-stream-iter-readable-interop.js` needs Buffer POOLING.** `bytes(...)` must
  resolve to a plain `Uint8Array`, but making `concatBytes`
  (`node_stream_iter_core.cppm:371`) reject Uint8Array *subclasses* in its
  single-chunk fast path greened it **and regressed
  `test-stream-iter-transform-sync.js`**, which deep-equals a `bytesSync()` result
  *against a Buffer*. Both pass on node only because `Buffer.from(str)` there is
  **pool-backed**, so a small Buffer never covers its whole ArrayBuffer and takes
  node's copy path, while zlib's exact-size output does. Unfixable inside the
  consumer. The lane reverted and left the reasoning as a code comment.
  **Worth sizing on its own:** pooling is why our `Buffer.from(str)` differs from
  node's in *every* identity/prototype-sensitive assertion, so siblings of this
  failure are likely elsewhere in both corpora.
- **`test-stream-writable-samecb-singletick.js` needs async_hooks `TickObject`
  instrumentation** — `createHook({init})` must fire exactly once with
  `type === 'TickObject'` across 100 `console.log`s; we emit 0 because
  `process.nextTick` has no async_hooks resource. The "exactly 1" arity makes it
  fragile even once instrumented.

### Cheapest untouched stream file
`test-stream-readable-compose.js` — one `Error: boom` escaping compose's error path,
whole-file failure at :111. `finished`/`finished-async-local-storage` and
`readable-async-iterators` all fail **late** in long files (20+ subtests pass first)
and are poor value per minute.

## 2026-07-30 10:30 — Buffer pooling: SIZED AND RETIRED (do not schedule)

Two lanes had raised pooling as a suspected explanation for identity-sensitive
Buffer assertions across both corpora. A sizing-first lane measured it and the
answer is **do not implement**. No code was written; the tree stayed clean.

**Mechanism confirmed — mbun has ZERO pooling.** `Buffer.poolSize` reports 8192 but
is **cosmetic**; every allocation gets its own exact-size ArrayBuffer:

| expression | mbun | node |
| --- | --- | --- |
| `Buffer.from("abc")` | byteLength 3, off 0 | byteLength 8192, off 0 |
| `Buffer.from("defg")` | byteLength 4, off 0 | byteLength 8192, off 8 |
| two `from()` share `.buffer` | **false** | true |
| `allocUnsafe(8)` ×2 share | **false** | true |

**Blast radius: 65 node source candidates → 1 genuinely blocked file. Bun: 0.**
The log gate did the work again (the campaign rule: source refs measure surface
area, logs measure blocked files). All 13 non-passing node candidates were read and
**none fails for a pooling reason** — they are OpenSSL message text, missing
`ERR_INVALID_STATE`/`ERR_INVALID_ARG_VALUE`/`ERR_INVALID_THIS` codes,
`parser.initialize` undefined, `util.inspect` formatting, publicExponent. On bun
every `pool` match was a **connection/worker/serializer** pool; the `8192` hits were
`S_IFCHR` and the `byteOffset` hit was a SQL error offset. `compat/bun/test/js/node/buffer.test.js`
— the only bun file touching `Buffer.poolSize` — is gated by **oom-kill**, not pooling.

**The one blocked file is a 1-for-1 swap, never a net gain.**
`test-stream-iter-readable-interop.js` needs `bytes()` to yield a plain Uint8Array.
Pooling is the sole discriminator: node pools `Buffer.from(str)` so it fails
`concatBytes`' identity check and copies, while zlib's exact-size output keeps the
identity path. In mbun both are exact-size. `test-stream-iter-transform-sync.js` is
**currently pass**, so a consumer-side patch trades one green for another — which is
what an earlier lane measured empirically before reverting.

**Cost that kills it:** node's pooling is *asymmetric* — `alloc` and
`allocUnsafeSlow` are UNPOOLED, only `allocUnsafe`/`from(string|array)` are pooled.
Getting that split wrong breaks assertions passing today (`test-buffer-alloc.js:43`
`b.byteOffset === 0`, `:1144` `allocUnsafeSlow(10).buffer.byteLength === 10`,
`test-buffer-slow.js`). Guard surface ~700 files, and it is a perf-sensitive global
hot path — the same shape as the abandoned full `util.inspect` replacement.
**~700 files of exposure plus allocator perf risk to buy one file.**

**Actions:**
- Reclassify `test-stream-iter-readable-interop.js` as **known-blocked / wontfix**,
  pointing at the existing comment at `node_stream_iter_core.cppm:371` so no third
  lane re-derives it.
- If ever revisited, the cheap door is **pooling `Buffer.from(string)` ONLY** — the
  single node path that creates the discriminator — leaving
  `alloc`/`allocUnsafe`/`allocUnsafeSlow`/`from(arrayBuffer)` untouched. Even then,
  only bundled into a broader Buffer-semantics project with an allocation-throughput
  gate, never standalone for one test.

### `test-stream-readable-compose.js` — isolated to a two-site handoff (not fixed)

The failure is **not** "compose loses errors". Probed both shapes:
```
Readable.from([1,2,3]).compose(async function*(s){ for await (const c of s) { throw new Error('mid'); } })
  -> toArray() REJECTS correctly
Readable.from([1,2,3]).compose(async function*(s){ for await (const c of s) {} throw new Error('after'); })
  -> escapes as an UNCAUGHT exception
```
So a throw **mid-stream** propagates; a throw **after the source drains** does not.

Path: `compose` → `Duplex.from(fn)` → `duplexify`
(`node_stream_writable.cppm:1041`) → `fromAsyncGen(body)` → `from(Duplexify, value, …)`
= `internal/streams/from` (`node_stream_core.cppm:1345`).

**`nextAsync` (`node_stream_core.cppm:1478`) looks CORRECT** — `await iterator.next()`
inside `try`, `catch (err) { readable.destroy(err) }`. So the rejection is very likely
escaping through **`fromAsyncGen`'s separate completion await** (the `final`/`write`
pair destructured at `node_stream_writable.cppm:1041`), not through the readable's
iteration loop. That is the next place to instrument.

Deliberately not changed: `internal/streams/from` and `fromAsyncGen` sit under **236
green `test-stream` files** plus http/fs/webstream consumers, and a speculative fix
in an async-iterator error path is exactly the shape that regresses many files at
once. Wants a lane that can guard `test-stream` + `test-http` + `test-webstream`
cheaply.

### Two bun files re-examined and DE-PRIORITISED with reasons (wave 54)

- **`internal/macos-cross-config.test.ts`** (18 pass / 1 fail). Its fix was lost as
  *collateral* when the bunfig-preload commit was reverted, so it looked like free
  re-application (as `path-ignore-patterns` genuinely was). It is not: the test
  parses `PACKED_FEATURES_LIST` out of the **vendored** `compat/bun/src/analytics/lib.rs`
  and requires `crash_handler.getFeatureData().features` to match it **exactly, in
  order**. So the "fix" is a hand-copied data table mirroring a Rust macro in
  read-only vendored source — a permanent sync hazard for one file. If it is ever
  wanted, **generate** the table from `compat/` at build time; do not hand-copy it.
- **`cli/bunfig-test-options.test.ts`** and **`config/bunfig/preload.test.ts`** are the
  same territory as the twice-reverted preload change. Do not re-attempt without a
  guard that includes the 5 files it broke both times (`only-inside-only`,
  regressions `14135`/`19875`/`20092`/`5961`) — they are named in the wave-46 and
  wave-48 entries.

## 2026-07-30 08:00 — WAVE 53 (solo): test-stream-consumers green, plus a sized queue

**`test-stream` 233 → 234/249**, 0 regressions (also clean on `test-webstream`).
Node overall ~2,924/4,433 (66.0%).

### `test-stream-consumers.js` green — and a near-miss worth remembering

Two defects. The second nearly cost a passing assertion:

1. A second consumer on an already-**locked** ReadableStream resolved instead of
   rejecting. `Bun.readableStreamTo*` throws a bare TypeError with **no `code`**, so
   a shared `assertUnlocked()` now raises `ERR_INVALID_STATE` before delegating.
   **I first guarded blob/arrayBuffer/bytes/text and missed `json`** — the file kept
   failing byte-identically, which looked like the guard not working at all. A probe
   proved it fired correctly for `blob`, so the fault had to be an unenumerated
   case. The test exercises **four** ERR_INVALID_STATE paths. Enumerate all cases a
   test touches, not the representative-looking ones.
2. `text()`/`json()` over an object-mode stream must reject `ERR_INVALID_ARG_TYPE`
   for a non-BufferSource chunk — but `blob()`/`bytes()`/`arrayBuffer()` must **NOT**:
   they stringify through Blob, and the *same file* asserts `bytes()` yields
   `'[object Object][object Object]'` (30 bytes). The tempting shared chunk guard
   would have flipped one assertion green and broken a passing one. Validation
   belongs only in `text()`, with `json()` inheriting it by delegation.

### The remaining 13 `test-stream` failures are 13 DISTINCT causes

Classified from logs (no shared root cause — the umbrella rule holds a sixth time):
`destroy` (mustNotCall fired), `finished-async-local-storage`, `finished`,
`iter-readable-interop` (deep-equal), `iter-transform-errors` (brotli code, below),
`pipeline-process`, `preprocess`, `readable-async-iterators`, `readable-compose`
(`Error: boom`), `wrap-drain`, `wrap-encoding`, `wrap` (below),
`writable-samecb-singletick`.

### Two items SIZED, deliberately not started solo

**1. Brotli error codes — and a bigger defect underneath.**
`test-stream-iter-transform-errors.js` wants `code: 'ERR__ERROR_FORMAT_PADDING_2'`
(node = `ERR_` + `BrotliDecoderErrorString()`, which returns a leading-underscore
name — hence the double underscore). mbun reports `Z_BUF_ERROR` /
"unexpected end of file", manufactured at `zlib_stream.cppm:355` whenever a decoder
has not ended.

**The real defect is worse than the code string: a CORRUPT brotli stream is being
reported as TRUNCATED.** `modules/compress/src/brotli.cppm:65-73` already
distinguishes `BROTLI_DECODER_RESULT_ERROR` (`invalid_input`) from
`NEEDS_MORE_INPUT` (`truncated_input`), but that distinction never reaches JS — the
streaming path in `modules/compress/src/stream.cppm` falls through to the
"unexpected end of file" branch. Fixing properly needs: the brotli error string into
`compress::Error::message` (the struct already has a `message` field, so **no ripple
through zlib/gzip/zstd consumers**), propagation through the streaming handle, and a
brotli-aware code in `zlib_stream.cppm`. Payoff ~1 node file + part of bun's
`js/web/streams/compression.test.ts`. Shared by four codecs across both corpora, so
it wants a lane with parallel verification.

**2. `test-stream-wrap.js` needs a real `internal/js_stream_socket` handle.**
The test does `req.handle = wrap._handle` then `req.handle.shutdown(req)`;
`wrap._handle` is **null**, so `StreamWrap` exposes no handle at all. Needs a
`JSStreamSocket` whose `_handle` supports `shutdown()` (and by extension the
`ShutdownWrap` oncomplete contract). Subsystem work, and it is the same machinery
`test-stream-wrap-drain`/`-encoding` need — so one project, three files.

## 2026-07-30 07:00 — WAVE 51/52: solo work under sustained API saturation

**Node 2,821 → 2,923 / 4,433 (65.94%), +102, zero regressions — and every one of the
4,433 rows is now backed by a measured guard run**, so the figure is no longer a
projection over a stale baseline.

### Five consecutive subagent dispatches died on server-side 529 Overloaded

Machine resources were fine throughout (mem 34-36G, disk 50G, load ~1); the
constraint was API capacity, which the resource watchdog cannot see. Response was
to work solo rather than keep burning dispatches. **If a future session sees
repeated 529s, do not retry in a loop — switch to integration-side work and retry
periodically.**

### `test-vm-module-errors.js` went green solo — EIGHT independent defects

It advanced 29 → 52 → 104 → 127 → 143 → 182 → 203 → 224 → green, and every step was
a different bug:
1. `options.identifier` unvalidated (`initBase` coerced `0` via `${identifier}`).
2. Relink reported `ERR_VM_MODULE_STATUS`; node distinguishes an in-flight link
   (STATUS) from a finished one (`ERR_VM_MODULE_ALREADY_LINKED`).
3. **`link()` settled SYNCHRONOUSLY** on a dep-free module, so an un-awaited
   `m.link(cb)` left status `'linked'` where node leaves `'linking'`. The
   synchronous shortcut had a **deliberate comment** — it existed so
   `await link()` observes `'linked'`, and it was load-bearing for 8 files a lane
   had just won. Moving the flip into a **microtask** preserves that (the awaiting
   continuation resumes after the callback) while restoring `'linking'`.
4. `namespace` guard rejected only `'unlinked'`, not `'linking'`; message must name
   both.
5. A linker returning a module from a **different context** was accepted, silently
   linking two realms (`ERR_VM_MODULE_DIFFERENT_CONTEXT`).
6. Linking onto an **errored** module succeeded; node fails with
   `ERR_VM_MODULE_LINK_FAILURE` carrying the dependency's error as `cause`.
7. Importing a name a dependency does not export was undetected. Per spec this
   resolves at **link** time → SyntaxError, not a runtime miss. Needed a new
   `kNamedImports` record checked *after* the recursion, because only then are
   children linked and `exportNamesOf()` complete.
8. `evaluate({breakOnSigint})` threw `Error` not `TypeError`; `cachedData` accepted
   anything.

**Generalisable: "one file, one blocker" rarely holds.** Every brief this campaign
that said "N files gated on X" undercounted; this file is the extreme at 8.
For `timeout` and `cachedData` only the **argument contracts** were implemented —
interruption needs real JSC interrupt support and bytecode cachedData is not
achievable; both remain DEFERRED and the commits say so.

### Two leads retired by probing instead of staffing

- **`[0,0,0,0]` async_hooks grep is thin.** A lane called it "a cheap grep for more
  of these". Corpus-wide it matches **5 files**, one of which (`test-http2-ping`) is
  already green. `test-http2-debug` is a from-scratch `NODE_DEBUG=http2` tracing
  item; `test-async-wrap-uncaughtexception` fails on `call_id` `null !== 2`, i.e.
  real `executionAsyncId` context tracking — a subsystem, not a wrap. The reusable
  `__mbunAsyncHookWrap` helper was worth exactly the one file it already won.
- **`Buffer.from(typedArray)` is NOT a codebase-wide landmine** (audited earlier):
  `socket.write(new Uint16Array([1,2,3,4]))` yields the correct 8 bytes, and
  `crypto_asym.cppm:119` already does `Buffer.from(v.buffer, v.byteOffset, v.byteLength)`.
  The http2 PING site was the exception.

### JS-buffer redirect in `Bun.$` — analysed, NOT started, with the exact contract

`js/bun/shell/commands/yes.test.ts`'s 3 failures are **timeouts, not assertion
failures**. The contract, from the test source:
```js
const buffer = Buffer.alloc(10);
await $`yes > ${buffer}`;
expect(buffer.toString()).toEqual("y\ny\ny\ny\ny\n");
```
So stdout redirected to a JS Buffer must write **at most `buffer.length` bytes, stop
the producer, and resolve** — against an infinite producer.

Punt site: `lower_redirect` in `modules/jsc/src/runtime/bunsh.inc:239`. `atom_literal`
returns nullopt for a non-literal (buffer) target, so the whole script goes to
`/bin/sh`, losing every mbun builtin. **This is a three-layer change** — lowering in
`bunsh.inc`, a new buffer-target `RedirectPlan` variant, and a stop-when-full writer
in `modules/shell`'s interpreter. Deliberately not started solo: a partial version
replaces machinery the 27-green shell dir depends on.

**Reach caveat for whoever takes it:** a naive `grep '> \$\{'` reports 46 files, but
that also matches ordinary **file paths** in templates. Do not size this from that
number — confirm the target is a Buffer/TypedArray, not a path string.

## 2026-07-30 05:30 — WAVE 50: +8 node, and a keep-or-revert condition honoured

`test-crypto` **86 → 101/129**, `test-http2` **213 → 223/272**, bun `shell/` **20 → 27 green**,
all with zero regressions. Lane goals: ckey **2**/≥2, h2d **2**/≥2, dhec 1/≥2, cp 11 assertions
(file not green — see below).

### The derive widening was reverted, as promised

`node_asym_okp_derive_host` (`crypto_asym.inc:1158`) is a generic `EVP_PKEY_derive` narrowed by one
`if` to X25519/X448. That narrowing is *why* `crypto.diffieHellman` sat on the do-not-attempt list
as "provider-absent" for several waves. Widening it to `EVP_PKEY_DH`/`EVP_PKEY_EC` measured **0
gains, 0 regressions**, and I retained it explicitly as a prerequisite with the condition: *if the
follow-up lane cannot use it, revert*.

Two consecutive lanes died on server-side 529s, so integration answered the condition directly with
a probe:
```
EC  agree: true 32              <- but EC already worked via AN.ecdhComputeSecret
DH  FAILED: deriveBits: derive setup failed
```
**Finite-field DH does not work through it** — `EVP_PKEY_derive_init`/`set_peer` fails, so
`asym_load_pkey` most likely cannot reconstruct DH params from the DER it is handed. The widening
bought nothing for either algorithm. **Reverted.** The do-not-attempt entry for finite-field `dh`
stands after all, but now for a *measured* reason rather than an assumed one, and the real blocker
is named: DH key loading, not the derive call.

### Corrections lanes made to briefs I wrote (wave 50 alone)

1. **`enableConnectProtocol` "one print away from the leak"** — there was **no leak**. The path
   worked; the bug was RFC 8441 §3 (a peer that advertised `ENABLE_CONNECT_PROTOCOL=1` may never
   send `0` again) plus the server continuing to accept `:protocol` after withdrawal.
2. **My PING retraction was half-wrong.** async_hooks *was* needed, but a second bug also gated the
   file: **`Buffer.from(typedArray)` copies elements, not bytes**, so `Uint16Array([1,2,3,4])` went
   on the wire as 4 bytes and drew `FRAME_SIZE_ERROR`.
3. **`cp.test.ts` is not gated purely on a missing builtin.** Its 30 tests are 15 cases run twice;
   14 failures are `(exec)` variants blocked on `bun run *.sh`. All 11 genuine `cp` assertions pass.
   The feared 839-line flag matrix is not exercised — only `-v` and `-R` are.
4. **`crypto.diffieHellman` was mis-listed as provider-absent** (EC primitive already existed), and
   **RSA `crypto.encapsulate` is not PQC** — it is RSASVE via plain OpenSSL 3, wrongly lumped with
   ML-KEM.

**Meta-lesson, and it is aimed at the briefing process:** *"Distrust 'one print away' handovers;
write the 15-line repro first."* A lane's closing diagnosis is one inference at the end of its
timebox, not a verified finding. Mark handoffs as **unverified leads** in the next brief.

### `Buffer.from(typedArray)` — audited, NOT a codebase-wide problem

A lane flagged it as a likely landmine. Integration probed instead of sweeping:
`socket.write(new Uint16Array([1,2,3,4]))` produces the correct 8 bytes `[1,0,2,0,3,0,4,0]`, and
`crypto_asym.cppm:119` already does `Buffer.from(v.buffer, v.byteOffset, v.byteLength)`. The http2
PING site was the exception. **No sweep needed.**

### The WeakMap-state class is now FIVE instances

Objects whose state lives in a WeakMap, handled by generic machinery that finds zero own
properties: `CryptoKey` via `structuredClone`, `KeyObject` via `structuredClone`, `KeyObject` via
`assert.deepStrictEqual` (two *different* secret keys compared equal), the typed-array `byteOffset`
native bridge, and `copyBytes`' `instanceof ArrayBuffer` check failing cross-realm. **Grep for this
deliberately** — it is not five coincidences.

### Shell builtin seam is CLOSED

With `ls`/`rm`/`mv`/`cp` landed, **no bun shell file is gated purely on a missing builtin**. Next on
that surface is `bun run <file>.sh`, which is deliberately unimplemented (`app.cppm:2001`,
`run_command.cppm:24` document defaulting to the system shell because `mbun.shell` lacks `$VAR`
expansion and the `exit` builtin) — a CLI-entry-path change needing a both-corpora guard.

### Runner hazards recorded
- `generateKeyPairSync('dh', {group:'modp18'})` (8192-bit) takes **>20s**; it flips
  `job-error-parity` fail→timeout under a 20s limit. Not a regression. Use `--timeout 60`.
- `bunshell.test.ts`'s `(fail)` name set is **not** always byte-identical run to run; the
  documented "diff the names" check alone produced a false regression for one lane. The
  **alone-re-run** is what settles it.

## 2026-07-30 04:00 — WAVE 49: the strongest wave of the campaign

**Node 2,821 → 2,916 / 4,433 (65.78%)** across the PR, +95 with **zero regressions**,
from **2,822 guard files actually measured**. Wave 49 alone moved three subsystems:

| subtree | wave-39 | now |
| --- | --- | --- |
| `test-vm` | 52/98 | **68/98** |
| `test-crypto` | 86/129 | **98/129** |
| `test-http2` | 213/272 | **220/272** |
| bun `shell/` dir | 20 green | **27 green** |

Lane goals vs actual: vmmod **8**/≥3, h2b **4**/≥2, cr2 **4**/≥3, coreutils **4**/≥3,
vm **4**/≥3, rawkey **2**/≥2, parsermeta (bug lane), sh 1/≥2.

### A source-corruption bug, and my hypothesis about it was wrong

`mbun -e 'const s = "x = import.meta;"; console.log(JSON.stringify(s))'` printed
`"x = __mbunImportMeta;"`. I briefed this as a parser bug at
`js_parser.cppm:5550`. **The parser was innocent** — its `add_edit` calls take
positions from real tokens and are structurally immune, proven by a required-module
probe that came back byte-identical.

The culprit was a legacy blind text pass on the **entry path only**,
`engine.inc:507` `transform_module_`: a `std::string::find`/`replace` over the whole
source with no notion of string literals. It was worse than reported — because the
parser has *already* lowered every genuine `import.meta`, that pass's guard was true
**only when the remaining occurrence was inside a string or comment**. It was dead
for correct code and live exclusively for the corrupting case. And when it fired it
also ran `strip_await_`, silently deleting **every `await`** in the file.

Deleted, along with three now-dead helpers. `test-vm` +16, `test-module` +1,
0 regressions, bun transpiler/resolve/shell/bundler sweep 67/67 green.

**Audit result:** there are now **zero** blind text substitutions over source on the
C++ side. The one remaining is the JS-side `DYNIMPORT_RE` in `node_vm.cppm`, a
deliberate trade-off. **The dangerous pattern to grep for is `strip_await_`-class
helpers, not `add_edit`.**

### Method worth propagating: prototype against the live native bridge

The `rawkey` lane implemented raw KeyObject export/import in **one build, no
round-trips**, by monkeypatching `globalThis.__mbunCryptoAsymNative` from user JS
until both target tests exited 0, and only then porting the proven JS into the
`.cppm`. Any lane working the JS-in-C++-raw-string layer can do this, and it
sidesteps the build lock that is our throughput ceiling.

### "One shared cause" was an umbrella for the FIFTH time

`vmmod`'s 14 files were briefed as "ordinary vm.Module semantics". They resolved into
**6 distinct causes** — export-list regex capturing one declarator, link() recursing
before resolving the whole request list, SyntheticModule evaluation needing to settle
immediately per tc39 `#sec-smr-Evaluate`, re-entrant evaluate needing
`ERR_VM_MODULE_STATUS`, namespace needing `Symbol.toStringTag` as a non-configurable
own key, and `importModuleDynamically` resolving to a namespace rather than a Module.
Fixing all six greened 8 files.

### The http2 "5-file cost" in the source comment was wrong

The comment said force-finishing open streams cost 5 files, which had frozen that
code. `h2b` separated it into **timing** (sweeping inside the teardown microtask
reorders the stream's own `'close'`/`'aborted'`) and **error injection** (destroying
with `ERR_HTTP2_STREAM_CANCEL` re-reports a death the test already saw). Injection
alone costs **8** files, named in the lane report; a bare `destroy()` one I/O turn
later costs zero. **Rule: settle, do not re-report.** Those 8 names are now a
mandatory gate for any http2 teardown change.

### Lanes corrected the briefing four times this wave

- `cr2`: my `setFips` item was **inverted** — node does not refuse it in a non-FIPS
  build (`TODO(richardlau)` in the test); mbun's throw is the bug.
- `vmmod`: my "7/7 bun vm files exit 0" did not reproduce (6/7 fail, all pre-existing
  — the child is spawned without `--experimental-vm-modules`).
- `coreutils`: refuted that `bunshell.test.ts`'s 97 failures were coreutils text (the
  `(fail)` name set is byte-identical), and found `yes.test.ts` mis-filed — `yes` is
  already a builtin; its 3 failures are JS-buffer redirect targets punting out of the
  interpreter at `bunsh.inc:239`.
- `sh`: measured the `$(...)` reach as 4 files, only 1 actually gated — so the
  `${{raw:}}` pattern did **not** repeat, and it pivoted rather than assume.

### Deliberate non-actions worth keeping

- `coreutils` left `cp`/`cat`/`mkdir`/`touch` falling through to GNU rather than ship
  partial builtins, because a half-written `ls`/`rm` **replaces the binary other shell
  tests' own helpers depend on** (`rm.test.ts` uses `ls -d`, `mkdir`, `touch`).
- `rawkey` returned `ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS` for RSA/DSA/DH raw export
  and for `raw-seed` rather than invent an encoding — the JWK bridge cannot represent
  DSA, and seeds belong only to the absent PQC providers.
- `test-http2-altsvc` is green but its response is still truncated; it passes because
  the stream is *settled*, not because the DATA arrives. Recorded as a separate open
  bug rather than banked.

## 2026-07-30 03:00 — WAVE 48: +9 node, +5 bun, 0 real regressions

**Node 2,876 → 2,885** (`test-crypto` 86→92, `test-http2` 213→216; +9 −0 over 401
guard files). **Bun 887 → 891** (+5 gains; the one "regression",
`cli/install/hosted-git-info/boundary-conditions`, is the known network flake —
it went fail/green/green/fail across the last four runs).

| lane | goal | actual |
| --- | --- | --- |
| cr (node crypto) | ≥3 | **6** |
| bb (bun js/bun) | ≥2 | **4** |
| bn (bun mixed) | ≥2 | 3 → **net +1 after revert** |
| h2 (node http2) | ≥3 | **3** |

### Dynamic adjustments made this wave

1. **Capacity rebalanced 4-bun → 2-bun/2-node** because the bun near-green pool
   thinned 108 → 42 as cheap wins were taken. Both node targets were chosen for
   the *mostly-green subsystem* shape (crypto 86/129 green, http2 213/272) that
   has paid best all session — and both delivered.
2. **A mid-flight watchdog** now samples resources every 60s while lanes run
   (floors: mem<8G, disk<15G, load>28), so an excursion is visible during a wave
   rather than only at the next dispatch. Wave 48 stayed at mem 35-37G, disk 38G,
   load ~2.5 — no excursion.
3. **Screen v7** added two filters that removed **47 of 89** candidates: drop
   `spawn node ENOENT` (no `node` on PATH — permanently unwinnable) and drop tests
   whose SOURCE contacts public hostnames. The second matters because a live
   `google.com` lookup made a file flip green with no code change; screening on
   failure text cannot see that, screening on source can.

### The bunfig preload change cost the same 5 files a SECOND time

A lane re-implemented it (`--config`, bunfig preloads under `bun test`,
`rerunEach`) and guarded it against 13 files, which passed. The specific 5 that
the *first* attempt broke — `only-inside-only`, regressions `14135`/`19875`/
`20092`/`5961` — were not in that guard and broke again. Reverted again: +4
gained vs 5 lost.

**Rule: when a change is re-attempted after a previous revert, its guard MUST
include the exact files the previous attempt lost.** Those file names are in the
wave-46 entry; anyone retrying this must run them. The failures are inline-snapshot
mismatches, so whatever the preload path perturbs, it reaches snapshot formatting.

### Findings worth more than the files

- **http2 post-error teardown, half-fixed and fully diagnosed.** `_teardown()`
  force-destroys only *pending* streams (`const pending = !this._connected ? … : []`)
  on the recorded grounds that force-finishing open streams "cost 5 files", so an
  open stream whose transport dies mid-response is left dangling with no
  `'error'`/`'close'`. That is the remaining reach into bun's grpc-js files. The
  other half — a `session.request()` from a close handler throwing *synchronously
  inside the emit* and unwinding the whole teardown chain — is fixed.
- **`${{ raw: … }}` was escaped as a single word**, so every raw-built shell
  command exited 127 corpus-wide. 44 bun files use it; the fix moved
  `bunshell.test.ts` from 120 to 96 failing assertions. Nobody had attributed
  those to it. The lane's lesson generalises: look for the shared defect under
  several near-green files rather than picking them off one by one.
- **Async crypto callbacks were re-invoked when the callback itself threw**
  (`try { fn(null,r) } catch(e) { fn(e) }`), which is why domain tests saw
  "Expected exactly 1, actual 2".

### Provisional result flagged by its own lane

`js/bun/test/test-only.test.ts` went green as a side effect of the shell raw-splice
fix and the lane did not verify which assertion it repaired. Treat as provisional.

## 2026-07-30 02:10 — WAVE 47: 4 lanes, all met or beat their numeric goal

Pre-dispatch gates now run every wave (this is the fix for wave 46, where a
toolchain flip cost three lanes a box each):

| gate | wave-47 reading | decision |
| --- | --- | --- |
| base builds | `build_or_die: ok — 607a9025567c80a3` | dispatch allowed |
| resources | 38 GB free, load 1.95, **disk 31 GB (98%)** | disk binding → reclaimed to 33 GB |
| stray runners | 0 | clean |

**Parallelism set to 4, not the permitted 5** — disk, not RAM, is the binding
constraint (each lane's `target/` grows and the box hit 98% twice). Record the
input, not just the number.

| lane | goal | actual |
| --- | --- | --- |
| n47 (bun js/node) | ≥3 | **4** (3 fixes + 1 flake it correctly refused to bank) |
| b47 (bun js/bun) | ≥3 | **3** |
| r47 (bun regression) | ≥2 | **2** |
| x47 (bundler+mixed) | ≥2 | 0 in-list, +1 out-of-list, plus the verdict below |

### The find of the wave: a silent data-corruption bug in the native byte bridge

`JSObjectGetTypedArrayBytesPtr` answers the ArrayBuffer **base**, while
`JSObjectGetTypedArrayByteLength` is **view-relative**. Every typed array with a
non-zero `byteOffset` crossing into native was therefore read from the wrong
bytes at the right length — corruption, not a crash.

The lane found it by refusing to read decompression code: a raw-socket dump of
the wire bytes located the corruption on the *server* side in ~4 minutes, and a
copy-vs-view A/B pinned it. It also explains the exact pass/fail split in both
files it fixed — `Bun.gzipSync` returns a fresh array and `Uint8Array#slice`
copies (offset 0, passed), while `node:zlib` returns a Buffer whose `.slice` is
`subarray` (offset ≠ 0, failed).

**Integration audited all 13 call sites**: 8 read caller-supplied views and are
now fixed (`bun_build`, `sql`, `sqlite`, `valkey_client`, `sourcemap`, bun:ffi
`ptr()` ×2, the ffi argument coercion); 5 write into freshly-allocated arrays
whose offset is always 0 and are correct as-is; ArrayBuffer paths need no offset.
**sqlite/sql/valkey/ffi are the ones that mattered** — a `buf.subarray(n)` bound
as a blob or handed to native code silently carried the wrong bytes.

### A cross-corpus conflict RESOLVED rather than traded

The http2 `SETTINGS_ENABLE_PUSH` fix (wave 46) *injected* `enablePush:false` into
the server's initial SETTINGS, making the frame 6 bytes where node sends an
**empty** one — which regressed node's `test-http2-settings-unsolicited-ack.js`
(it deep-equals the raw frame). Narrowed to **clamp a caller-supplied value, never
inject one**: an empty frame already satisfies bun's `29073`. Both now pass —
`test-http2` 213/272 (exactly baseline, +0 −0) and `29073` green. This is the
third conflict shape: not "route through `__bunStyle`", not "irreducible", but
**the bun requirement was weaker than the implementation assumed**.

### Verdict: "the bundler" is at least FOUR projects, not one

Three lanes had reported bundler failures as one output-format cause. A lane
checked and refuted it: its 7 files fail in 6 independent subsystems —
output-format/chunking, **a CSS color model that does not exist at all**,
build-time macros, metafile/compile, TS-syntax rejection, macro error reporting.
Note the CSS file's 24 "passing" tests pass **by accident**: mbun echoes anything
unfoldable verbatim, which happens to match the expected `lab()` rows. Best
value-per-line item found: port `map_gamut`/`delta_eok`/`gam_srgb`/P3→XYZ from
`compat/bun/src/css/values/color.rs` (~300 lines), which would green a whole file
and likely several `css/wpt/*` siblings.

### Screen corrections 6 and 7 (from lane evidence)

6. **Drop `spawn node ENOENT` / `Node.js not found in PATH`.** There is no `node`
   on PATH, so those files are permanently unwinnable — 4 of n47's 23 (17%).
7. **Live-network tests can pass spuriously.** `js/node/dns/node-dns.test.js`
   flipped green with no code change: it does a real `google.com` round-robin
   lookup. The existing `ENOTFOUND|getaddrinfo` filter misses it because the
   lookup *succeeds*. Screen on the test SOURCE touching public hostnames, not on
   failure text — and never bank such a flip as a win.

### Two cross-corpus traps recorded before they cost anything

- `getStringWidth('👨‍👩‍👦‍👦')`: bun wants 2, node's
  `test-readline-promises-interface.js` wants 8. That node file is **skipped**
  today, so the ZWJ fix is free — if it is ever unskipped this becomes real.
- `'gc' in globalThis` must be false without `--expose-gc`, but the lazy `gc`
  accessor is installed unconditionally (execArgv is not populated when builtins
  evaluate) and `in` does not invoke a getter. Fix by deleting the property once
  execArgv exists — do **not** drop the accessor, node depends on
  `typeof gc === "function"` under the flag.

## 2026-07-30 01:00 — TOOLCHAIN INCIDENT + wave 46

### The incident: the global toolchain flipped to clang mid-session

**Three lanes reported "the tree does not build at its base commit" and I initially
doubted them, because the main checkout had built fine minutes earlier. They were
right; I was wrong.** The main checkout only appeared healthy because `main.o` was
cached — `touch src/main.cpp` reproduced the failure immediately.

Root cause: `~/.mcpp/config.toml` had `[toolchain] default` flipped from
`gcc@16.1.0` to **`llvm@22.1.8`** (file mtime 23:34, mid-session, by something
outside this session). Two symptoms, one cause:

1. `src/main.cpp:241` — `const std::size_t count{… ? 2 : 1}` is a hard
   `-Wc++11-narrowing` **error** under clang, a non-issue under gcc.
2. ~15 undefined `WTF::`/`JSC::` symbols at link. The mangling is the proof:
   `libWTF.a` defines `_ZN3WTF21numberToStringAndSizeEdRSt5arrayIcLm124EE`
   (libstdc++) while the clang objects want
   `_ZN3WTF21numberToStringAndSizeEdRNSt3__15arrayIcLm124EEE` (libc++'s `St3__1`).
   The vendored JSC prebuilt is a libstdc++ build; libc++ can never link it.

**Fix:** restored `default = "gcc@16.1.0"`, keeping `default_target = ""`. Do NOT
copy `config.toml.bak` wholesale — its `default_target = "x86_64-linux-musl"`
would change the triple and invalidate every incremental artifact under
`target/x86_64-linux-gnu/`. Old value saved at `~/.mcpp/config.toml.clang-flip-20260729`.

**Measurements were not invalidated:** `build_or_die` returns the same fingerprint
`607a9025567c80a3` as before the flip, so everything reported earlier was produced
by the gcc binary.

**Protocol change: verify the wave base builds BEFORE dispatching lanes.** Two
lanes burned 40-minute boxes rediscovering this independently, and a third lost
its measurement entirely. One `build_or_die.sh` run at dispatch time costs ~60s
and would have saved all three.

### Wave 46 results: +4 bun, 5 recovered, 0 regressions

The three blocked lanes' code was salvageable — I built and measured it here:
**gains** `js/bun/net/tcp-server` (live `getpeername` accessors),
`js/bun/resolve/import-meta-resolve` (builtins bypass the on-disk resolver),
`js/node/url/url.test.ts` (WHATWG file-host Windows-drive quirk),
`js/third_party/grpc-js/test-channel-credentials` (timer-queue fix, below).

**Two reverts, both forced by measurement:**

1. **`url.parse` leniency — a cross-corpus conflict the lane missed.** It made a
   non-numeric port lenient to satisfy bun's `url-parse-format.test.js`,
   predicting from a grep that no node test asserted the throw. Measuring it
   turned node's `test-url-parse-invalid-input.js` **pass → fail**. Restored the
   throw; the conflict is now documented at the site in `bootstrap.cppm`. No
   caller-side discriminator exists, so `__bunStyle` routing cannot resolve it.
2. **bunfig "run preloads under `bun test`" cost 5 bun files.** The full run showed
   5 green→non-green (`only-inside-only`, regressions `14135`/`19875`/`20092`/`5961`).
   I first suspected the timer-queue change (the other high-blast-radius edit) and
   **disproved it by reverting**: those 5 still failed without it, and the revert
   only lost its own gain. Reverting the preload commit restored all 5. Collateral:
   an unrelated one-line `path-ignore-patterns` message fix rode in the same
   commit and was lost with it — worth re-applying on its own.

**`seq` was already implemented** (`modules/shell/src/interpreter.cppm:700`,
full BSD `-s/-t/-w`). The brief was stale. The real cause of its 4 failures is that
a `,` argument makes `Bun.$` punt the whole script to `/bin/sh`, so GNU `seq` runs
— now fixed (27→30 assertions), with the last one needing command substitution.

**The timer-queue fix is genuinely infrastructural**: `__mbun_timers_reset` dropped
`T.q` wholesale, so **any corpus file with a top-level `setTimeout`/`setImmediate`/
`fs.readFile` awaited from inside a test never fired** — in both corpora. That is
why one grpc file failed while 21 siblings passed: it was the only one with
module-scope async.

**"test timed out" is a symptom class, never a cause class.** In the grpc cluster
alone it covered a test-runner bug, a `dns.lookup(all)` gap that never merges
families, an http2 post-error teardown gap, and a test that needs outbound network
(`test-tonic` downloads protoc and cargo-builds a Rust server — exclude it, like
`test-end-to-end`).

## 2026-07-30 00:30 — WAVE 45 (bun-focused): +8 bun green, 0 node regressions

Four lanes on the corrected `worklists-bun3` cut: **bnode +3** (assert partial
array match, crypto oneshot, crypto invalid-this), **bbun +3** (spawn NUL
rejection, node-shaped require, `Bun.jest`), **breg +1** (http2 server must never
advertise `SETTINGS_ENABLE_PUSH`), **bweb +1** (URLSearchParams duplicate-key
grouping + BroadcastChannel surface).

Node sweep after integration: `test-require` 16/23, `test-module` 15→16/32,
`test-path` 16/17, `test-crypto` 86/129, `test-assert` 5/14, `test-esm` 0/2 —
**+1, 0 regressions over 217 files**, including the riskiest change of the wave
(`require` changed from `function (s,o,k)` to `(s, ...rest) =>` in
`engine_require_js.inc`, i.e. every module's `require` in both corpora).

### The bun near-green screen, after FOUR corrections

Each correction came from a lane measuring the previous one wrong:

1. `failed ≤ 2` — **wrong**: a 1-fail/0-pass file died at *load*; its single
   assertion measured a whole missing subsystem.
2. `passed > 0` — better, but does not separate semantic failures from
   **resource-profile** ones.
3. `failed ≤ 5 AND pass-ratio > 0.5` — cleaner, but a memory/GC/threading budget
   is *by construction* a single assertion at the end of a long green file, so
   this filter actively selects **for** the worst traps. One lane's 4 of 10
   `js/web` candidates were exactly this.
4. **Current rule**, and the one to cut the next worklist with:
   - require `errors == 0`, not just low `failed` — `js/node/util/util.test.js`
     had 2 fixable failures but can never go green because an out-of-test `node`
     subprocess exits 127 (there is no `node` on PATH);
   - **subtract** any candidate whose failing assertion text matches
     `/memory|bytes|copies|objectTypeCounts|SharedArrayBuffer|heap|timed out|<run wedged>/`;
   - **subtract** rows whose log carries an absolute tmp path (`sun_path` 108-byte
     unix-socket limit makes files fail purely from a deep `--out` directory —
     `js/bun/http/bun-serve-args` loses 3 assertions to this alone);
   - prefer failure text matching `is not a function` / `Received: undefined` /
     `ERR_*` mismatches — one lane's 3 wins were all a missing-or-wrong
     **argument-shape** detail;
   - rank by **distinct root causes**, not failure count.

### Cross-corpus: one conflict resolved, one irreducible, one open

- **Resolved by routing** (the pattern to reuse): `URLSearchParams` inspect —
  node pins `URLSearchParams { 'a' => 'a' }`, bun pins a block form. Routed
  through the existing `__bunStyle` flag; both stay green.
- **IRREDUCIBLE — strike `js/node/path/to-namespaced-path.test.js` from bun
  worklists.** `path.win32.toNamespacedPath("\\\\?\\foo")`: bun asserts a trailing
  separator, node 26 `test-path-makelong.js:82` asserts none. Same API, same
  input, opposite values, and no caller-side discriminator exists. A lane
  implemented the bun side, measured 4/32 green, found it cost node's
  `test-path-makelong` **and** `test-path-resolve` (both previously green), and
  reverted. Decision is documented in a comment at the special case in
  `bootstrap.cppm` — do not let another lane re-derive it.
- **Open, needs a routing decision:** `new Worker("file:///…")`. Bun's *global*
  `Worker` accepts a `file://` href; node's `worker_threads.Worker` must throw
  `ERR_WORKER_PATH`. mbun installs one class for both
  (`node_worker.cppm:1573`). The `__bunStyle`-shaped fix is a separate global
  `Worker` subclass with the relaxed resolver, leaving `node:worker_threads`
  strict. Unblocks 2+ bun files.

### Highest-leverage uncashed items, ranked

1. **`.stack` has no `Name: message` header line.** JSC gives frames only; V8/bun
   prepend `Error: msg`. It is the entirety of both `third_party/express` files
   and is a plausible long tail across BOTH corpora. Wants a dedicated lane with
   a full-corpus before/after — too broad for a 40-minute box.
2. **grpc-js HTTP/2 streams never settle** — 5 bun files, one shared cause, pure
   JS (not the napi blocker).
3. **A real `seq` shell builtin** — `js/bun/shell/commands/seq.test.ts` is 27
   pass / 4 fail, all one cause: mbun has no `seq` builtin so it inherits GNU
   semantics from `/usr/bin/seq`, while bun implements BSD semantics.
4. **`__bunStyle` hook in `node_http.cppm`** — absent today; gates
   `regression/34415` and probably the whole HTTP/1.0-framing seam.

### `js/third_party/*` — exclude from per-file volume lanes

Two lanes scored **0 of 17** and **0 of 7** there. But the reason is useful: those
7 files carry exactly **two** root causes (5× grpc-js, 2× the `.stack` header).
They are a poor unit of work and a good *signal* of which subsystems unlock the
most files at once.

## 2026-07-29 23:40 — BUN AUTHORITY RE-ESTABLISHED: 866 / 1902 green on the current tree

`target/integration/w44-bun-full` — full 1902-file bun corpus, current integration
binary: **866 green, 874 test-failure, 32,146 pass / 16,456 fail assertions.**
**Use this as the bun baseline from now on**, not the 230-file focused set and not
the 2026-07-21 `r5-bun` run.

**Nine bun files verified green this wave** (9/9 of what the lanes claimed,
re-checked in the full run, not taken on report):
`regression/14477`, `regression/16476`, `js/node/diagnostics_channel`,
`js/bun/crypto/x25519-derive-bits`, `js/bun/stream/direct-readable-stream`,
`js/node/perf_hooks`, `js/deno/fetch/headers`, `js/web/crypto/web-crypto`,
`js/web/url/url`.

### The 61-file caveat, stated plainly

`corpus_diff r5-bun -> w44-bun-full` reports **61 green→non-green and 43
non-green→green** (884 → 866). This is **not** a clean attribution and must not be
quoted as "we regressed 61 bun files":

- `r5-bun` is from **2026-07-21** — eight days and many waves old, and it predates
  the measurement-honesty fixes (`assert.throws` validating its error argument,
  `mustCall` enforcement, self-skips excluded). Those made the node number fall
  legitimately; the same correction applies here.
- The runner's classification buckets changed underneath: `fixture-build-error`
  went 0 → 12 (the bucket did not exist), `crash` 13 → 1, `ahead-of-reference`
  0 → 3. Files moved between buckets without their behaviour changing.

What WAS checked: the one file on that list this wave could plausibly have caused
— `js/node/async_hooks/EventEmitterAsyncResource.test.ts`, because a lane changed
`AsyncResource#bind` arity — fails on `triggerAsyncId reflects the option`, and
`git show 5928f18 | grep -c triggerAsyncId` is **0**. Unrelated and pre-existing.
The remaining 60 are unattributed; with `w44-bun-full` as the baseline the next
wave can diff same-tree and settle it properly.

**Lesson for the protocol: the consolidated integration sweep must cover BOTH
corpora.** This wave's sweep covered 513 node files and zero bun files, which is
how a bun-side regression could have slipped through unseen.

## 2026-07-29 23:05 — BLOCKER: mbun exports 6 dynamic symbols, so no napi addon can load

**Highest-ROI item found on the bun seam, and it is an UPSTREAM mcpp bug — do not
re-investigate it in this repo, route it to whoever owns mcpp.**

```
nm -D --defined-only <mbun> | wc -l          ->  6
nm    --defined-only <mbun> | grep napi_create_promise  ->  present (T)
nm -D --defined-only <mbun> | grep napi_create_promise  ->  ABSENT
```

The whole Node-API surface is compiled into mbun but lives only in the static
symbol table. A `dlopen`'d addon resolves `napi_*` against the host executable,
so **every prebuilt `.node` in both corpora fails to link** — sharp,
`@napi-rs/canvas`, prisma, swc, msgpackr, resvg, rollup's native binding. One
lane measured 4 of its 17 files gated on exactly this.

`mcpp.toml:28` already declares `ldflags = ["-Wl,--export-dynamic"]`. **mcpp
0.0.109 does not honour user `ldflags` at all.** Verified exhaustively — the flag
never reaches the generated `build.ninja` from any of:

- the root `[package]` (line 28, pre-existing);
- `[targets.mbun]`;
- a workspace member (`modules/napi`) — which is additionally not a root
  dependency, so it could never reach the link anyway;
- a *direct root dependency* that owns the napi seams (`modules/jsc`).

Corroborating evidence that the key is simply unimplemented: `modules/toml`
declares `ldflags = ["-fsanitize=address"]` and that does not reach the link
either. The `-ldl` / `-l:libssl.a` entries in build.ninja's global `ldflags` come
from toolchain/xpkg metadata, **not** from any mcpp.toml. `mcpp build` exposes no
`--ldflags`, and the only other lever is the machine-global `~/.mcpp/config.toml`,
which agents must not touch (it has been broken twice that way — see the
toolchain-mismatch note in the resume steps).

All four experiments were reverted; the tree is unchanged. The declaration at
`mcpp.toml:28` is left in place because it records the correct intent.

**Prerequisite already landed:** `141afe6 fix(napi): resolve glibc compat sonames
for dlopen'd addons`. Without it the addons fail earlier, at library resolution,
and the symbol problem stays invisible.

## 2026-07-29 22:20 — STRATEGY CORRECTION: pivot capacity to the BUN corpus

The maintainer flagged that throughput was slow **and that bun is half the
mandate**. Both are right, and they are the same problem: waves 40–42 spent
every lane on the node corpus and touched bun zero times, while node's long tail
costs a lane 1–3 files.

**The bun corpus is far cheaper per green file right now.** From the wave-38 full
bun run (`target/integration/codex-sprint2-wave38-full-bun`, 230 files, 93 green,
120 test-failure, 740 failing assertions):

- **75 files need only ≤2 failing assertions fixed to become fully green**;
- 93 need ≤5;
- **59 of them produce ZERO passing assertions** — the file dies at load, which is
  usually one missing export or API away from green.

Near-green worklists cut by area into `target/integration/worklists-bun/`:
`js-third_party` 17 files/17 assertions, `js-node` 14/22, `regression` 12/14,
`js-bun` 10/13, `integration` 7/10, `js-web` 6/10, `cli` 4/5, `js-valkey` 3/8,
plus singles. **84 files needing ~113 assertion fixes in total.**

Bun measurement recipe (differs from node — no `--bin auto` guard covers it, so
resolve the newest binary by mtime yourself):
```
python3 tools/integration/bun_corpus_runner.py --bin <newest> --cwd compat/bun \
  --list <worklist> --out <dir> --jobs 4 --timeout 30
```
`--cwd compat/bun` is mandatory. `all-skipped` / `no-tests` / `blocked-external`
are not passes; `ahead-of-reference` means mbun is *more* correct than bun's own
reference and is not a failure.

### Throughput fixes adopted at the same time

1. **Lanes no longer run wide subsystem guards.** Integration was already running
   a consolidated sweep over every lane's surface, so lane-level guards were
   duplicated work — and they were consuming roughly half of each lane's box
   (two lanes blew a 30-minute box on guard runs and build latency alone). Lanes
   now measure only their own list and declare any high-blast-radius touch so
   integration widens its own sweep.
2. **Integration lands fully-diagnosed ≤5-line fixes directly** rather than
   spending a lane plus two builds (this is how the CryptoKey `structuredClone`
   fix landed for free).
3. **Batch edits per build.** The shared build lock is the ceiling; a lane fixing
   8 small files should build once, not eight times.

## 2026-07-29 22:05 — WAVE 42 FINAL: +55 cumulative (40+41+42), 0 regressions

Node **2821 → 2876 / 4433 (64.88%)** from **1612 guard files measured**, zero
green→non-green anywhere. PR #35. Throughput **21 files/hour at 4 lanes** over
2.6h (wave 40 ran 32/h on unmined clusters; the decline is the tail, as expected).

| lane | added | commit |
| --- | ---: | --- |
| tlstriage | +3 | CA store: extra-cert newline, default⊇extra, element validation (`dd98a7a`) |
| preparestack | +2 | `Error.prepareStackTrace` with real CallSites (`1c8df96`) |
| depaudit | +2 | restore DEP0111/DEP0119/DEP0144 emissions (`9512099`) |
| stdinthrow | +1 | DEP0005 for `new Buffer()` outside node_modules (`3f38219`) |
| testreporter | +1 | one reporter stream under `--test` (`189a23f`) |
| stdinthrow | 0 | `child.stdin` write-callback throws now propagate (`7923aab`) |
| replnav | 0 | REPL close waits for the pending history flush (`1fc441a`) |
| testreporter | 0 | spec reporter `Error:` prefix (`b49a9c0`) |
| inspecthard | 0 | survive `Array.prototype[Symbol.iterator]` deletion (`45991a6`) |
| testuncaught | 0 | no code — gate closed it |

### On the four retained zero-file commits

The protocol says additively revert zero-yield work. These were kept under the
narrower rule the wave-41 lanes operated by: **a change is retained at zero yield
only if it is provably correct against node's documented behaviour, has a
measured before/after probe, and guards clean.** All four do, and two of them
(`child.stdin` throws, `process.stderr.write` fragility) are correctness fixes on
paths that can *hide* other failures. This is recorded explicitly rather than
left to accumulate silently — if a future integrator disagrees, revert them as a
set, they are independent.

### Wave 42's real product: three retired questions

Negative results dominated this wave, and the log-first gate is why they were
cheap rather than expensive.

1. **tls is retired as a cluster target.** Of 62 "fixable" files, 6 are behind
   the self-declared DEFERRED `tls.connect` transport by explicit throw, and ~16
   more (SNI context switching, PKCS#12/PSK/OCSP identity, ticket resumption)
   fail *inside* the handshake — the same deferred `SSL_CTX` engine reached via
   `tls.createServer` instead. So ~22/62 (35%) are one deliberate project. 6 more
   sit on the shared socket/stream handle path that produced wave-32's 41-file
   cross-subsystem regression. The remaining ~34 are one-file-per-fix; bucket J
   alone is 12 unrelated one-offs. The CA-store bucket was the last multi-file
   cheap cluster and it yielded 3.
2. **The `node:test` uncaughtException handler is not a throughput lead.** It is
   genuinely missing, but it gates exactly **one** corpus file, because the
   `fixtures/test-runner/output/` family that exercises it is not vendored here
   (`test-runner-output.mjs` is absent). Correctness work, not coverage work.
3. **The inverted-deprecation-gate bug did not repeat.** 15 DEP sites audited, 0
   inverted. The one other site with that literal shape (DEP0104) is *correct* —
   node genuinely makes it `--pending-deprecation`-only, and "fixing" it would
   have been a bug. A different failure mode surfaced instead (2 emissions wholly
   missing, 1 half-wired), worth the +2. Residual, unfixed because it blocks
   nothing today: DEP0198/0203/0204 lack node's once-per-code dedupe and
   over-emit.

### Two diagnostics that make future sizing cheaper

- **A bare `Uncaught exception` with no detail in a corpus log means
  `process.stderr.write` itself failed.** That signature appears across several
  unrelated failing tests and was previously unreadable. The wrapper that broke
  it under prototype tampering is fixed, so the channel is one site less likely
  to lie.
- **Wrapper seams are where primordial discipline leaks.** `util.inspect`'s core
  was already hardened with captured primordials; the breakage was in a wrapper
  installed *over* it by an unrelated module (Headers formatting) using spread.
  Check wrappers before re-auditing an already-hardened core.

### Method now proven over three waves

**Gate on failure logs, not source greps.** Every lane that applied it produced a
correct sizing; every lead that skipped it was over-optimistic. The scoreboard:
`emitWarning` 73 refs → 28 non-passing → **0** blocked; `prepareStackTrace` 8 →
7 → **5** blocked (2 moved); prototype-tampering 50 → 9 → **2** blocked; `argv0`
12 → 7 → **2** blocked; `node:test` reporter "several" → **1**. Source references
measure surface area; failure logs measure blocked files.

**The shape that pays is "written but structurally unreachable."** Four for four
now: `internal/repl` resolving into node's lib chain; one missing
`internalBinding('crypto')` export taking down the whole `internal/crypto` graph;
`v8.promiseHooks` stubbed to a no-op; and `prepareStackTrace` scaffolding that
could never fire because this JSC has no `stack` descriptor on `Error.prototype`.

### Next candidates, ranked

1. **Module-loader primordial hardening** — one more `Reflect.apply`-shaped fix
   in the native module-load path closes `test-require-delete-array-iterator` and
   `test-repl-unsafe-array-iteration` (both blocked by the *same* remaining site).
2. `--test-reporter-destination` — genuinely unimplemented, the only multi-file
   reach left in `node:test` (`force-exit-flush` + a chunk of `reporters.js`).
3. `util.inspect` `[AsyncFunction (anonymous)]` tagging — first blocker of
   `test-util-inspect.js` at line 41.
4. `process.stdout`/`stderr` as real `Writable`s — large, silently caps `test-tty-*`.
5. DEP once-per-code dedupe, bundled with whoever fixes the PBKDF2 message.

## 2026-07-29 21:15 — WAVE 41 FINAL: +46 cumulative (40+41), 0 regressions

Node **2821 → 2867 / 4433 (64.67%)**, projected from **1127 guard files actually
measured**, zero green→non-green anywhere. PR #35. Combined throughput over both
waves: **24 files/hour with 5 agents over 1.9h** (wave 40 alone ran 32/h; the
drop is build-lock contention, below).

| lane | added | commit |
| --- | --- | --- |
| unhandled | +8 | `--unhandled-rejections` modes (`da89e3f`) |
| repl3 | +3 | ported node's `ReplHistory` manager (`59a2a97`) |
| argv0 | +3 | real `argv[0]` (`c31acf1`) + vm stack decoration (`c96d535`) |
| webcrypto | +2 | `QuotaExceededError` + key-class bindings (`fc0cb20`) |
| runner2 | +2 | `NODE_TEST_WORKER_ID`, `t.plan` wait (`2804c6b`, `710a005`) |
| cryptokey | +1 | spec-shaped CryptoKey slots (`5a1c354`) |
| *integration* | +1 | CryptoKey `structuredClone` re-mint (`42efe39`) |
| trace | +1 | publish the trace phase table (`1b0c26b`) |
| identity | +1 | drop `--` from `execArgv` (`a0c877b`) |
| emitwarning | 0 | node-shaped warning stack (`b4d6d75`) — kept as correctness |
| streamthrow | 0 | measurement-integrity probe, no code |

### The two findings that outrank the files

1. **No false greens in the stream-callback class.** The repl lane reported that a
   throw inside a `Writable` write callback was swallowed with exit 0 — the same
   shape as the `assert.throws`/`mustCall`/self-skip defects that once removed
   126 unreal passes. A dedicated lane **refuted it**: every generic stream path
   (write cb, `_write`, `'data'`/`'end'`/`'finish'`/`'close'`, nextTick,
   microtask, immediate, timers) propagates correctly. Corpus exposure was 1
   passing file, verified real by negative control. **The 2821 baseline is not
   inflated.** Two genuine bugs were re-filed as ordinary correctness work:
   `process.stdout`/`stderr` are not real `Writable`s (will cap the `test-tty-*`
   subtree later), and `child.stdin.write(chunk, cb)` does swallow a throw.
2. **Screen on failure logs, not source greps.** The `emitWarning` lane found a
   real, genuinely cross-cutting divergence (JSC-shaped warning stack instead of
   node's `Name: message\n    at …`) and it moved **zero** files, because no
   corpus test asserts on a warning's stack. 73 files referenced the API, 28 were
   non-passing, **0** were blocked by it. Reading those 28 failure logs takes two
   minutes and would have predicted the zero before two rebuilds. The sibling
   `identity` lane measured the same asymmetry: `execPath` has 281 non-passing
   references and zero divergence. **Source references measure surface area;
   failure logs measure blocked files. Gate on the logs.**

### Retired veins (do not re-fund)

- **Runtime-identity symbols.** Ten screened (`title`, `execArgv`, `ppid`,
  `release`, `config`, `version`, `versions`, `hostname`, `execPath`,
  `arch`/`platform`). Only `execArgv` cleared the gate (+1). `process.title` is
  worth at most 1 file and needs argv-area/prctl rewriting. The rest either do
  not diverge or would require mbun to reproduce **node's build manifest**
  (dependency list, ABI pins, `-node.N` V8 string) — not honestly passable.
- **Broad trace_events.** 2 files are unsatisfiable on JSC (`cat:'v8'` with
  `V8.*` names — no honest source; passing them means inventing event names), 2
  are permanently skipped, and 11 each need a different subsystem's
  instrumentation seam. Only two bounded items remain (`api-worker-disabled`,
  `console`), worth ~2 files.
- **webcrypto "as a cluster".** The cluster reading failed twice running: one
  lane predicted 3 and paid 1, the next predicted 2–4 and paid 1. Remaining
  failures are individually priced. 8 of 20 are blocked on algorithms the
  vendored OpenSSL lacks (TurboSHAKE, ML-KEM/ML-DSA, Argon2) — remaining work,
  not lane work.

### Operational: build-lock contention is now the throughput ceiling

At 5 concurrent lanes the shared `build_lock.sh` serialises every build, and one
lane measured **~12 minutes of queue wall-clock** for a single slot; two lanes
blew a 30-minute box on build latency alone, not analysis. Wave 40 ran 32
files/hour, wave 41 ran ~19. Options for wave 42, in preference order: (a) run
**4 lanes** rather than 5, (b) prefer lanes whose targets share a subsystem so
one build serves two items, (c) have integration land fully-diagnosed ≤5-line
fixes directly instead of spending a lane plus two builds on them — that is how
the CryptoKey `structuredClone` fix landed here for free.

### Next candidates, ranked by the evidence above

1. `uncaughtException` → running-test attribution in `node:test` (shared
   mechanism; the runner2 lane worked around it and expects >1 file).
2. REPL line-editor fidelity — but per-file, using the standalone-probe method
   the repl3 lane validated (extract one sub-test, seed its precondition, diff
   chunk-by-chunk; ~4 min/sub-test). Budget **one file per lane**.
3. `child.stdin.write/end` callback throw propagation (small, real false-green
   vector, land before any child_process lane).
4. `new Buffer(10)` emits no DEP0005 at all — blocks 2 files, but needs a wide
   guard because it changes stderr for every test calling `new Buffer`.
5. `process.stdout`/`stderr` as real `Writable`s — large, but it silently caps a
   whole subtree.

## 2026-07-29 20:35 — WAVE 40 FINAL: +24 Node green, 0 regressions

Eight lanes total (five dispatched, three re-tasked as lanes freed). Integrated on
`agent/corpus-coverage-w40`, PR #35. Build green; conflict-marker and
submodule-gitlink checks clean.

| lane | list | retained | commit |
| --- | --- | --- | --- |
| repl2 | 0→7 /12 | serve `internal/repl` from mbun's own REPL | `7a09587` |
| promise | 0→5 /5 | `v8.promiseHooks` lifecycle hooks | `7405e30` |
| vm | 0→2 /8 | sandbox writes to realm builtins | `b7d4131` |
| runner | 0→1 /14 | `node:test` tags + `t.plan` validation | `24bc978` |
| compile | 0→1 /15 | compile-cache disable trace | `4c26987` |
| deadblob | — | loud builtin-partition errors | `d42c5cc` |
| repl | 0/12 | reverted — triaged the seam for repl2 | — |
| trace | 0/8 | reverted — finding later falsified | — |

**Guards, per-file against the wave-39 full run, 677 files: +24 gained, 0 lost.**
`test-repl` 56→70, `test-promise` 6→11, `test-vm` 52→54, `test-runner` 29→30,
`test-compile` 1→2, `test-async` 22→23, and flat on `test-v8` 8/23,
`test-module` 15/32, `test-process` 66/96, `test-worker` 92/143.
Node projection **2821 → 2845 / 4433 (64.2%)**, pending the next full run.

### Computed throughput after wave 40

`wave_report.py --hours 0.75 --agents 5` on the projected tree
(`target/integration/w40-projected`, the wave-39 run with all 677 guard results
overlaid — a projection, NOT a full run):

```
wave: +24 gained, -0 regressed, net +24
throughput: 32 files/hour over 0.8h with 5 agents (4.8 files/agent)
remaining: 1026        time to 100% at this rate: 32 hours
```

**32 files/hour is the best rate the campaign has measured** (previous best 28
with 3 agents; waves 36–38 ran 11–20). Treat it as a ceiling, not a trend: it
came from clusters nobody had mined yet, and those deplete. `remaining: 1026` is
fail+timeout+oom; the 562 self-skips are real gaps in a separate bucket and are
not in that number.

### THE lesson of wave 40: three lanes lost to one stale binary

`mcpp build` keys its output dir on a config hash, so a checkout accumulates
several `target/<arch>/<hash>/bin/mbun`. All of them run. It cost three lanes in
three different shapes:

1. a lane measured a stale *hash directory* and got a false zero delta;
2. a lane measured a stale build of the right directory, same result;
3. **the trace lane reasoned from probes run against a Jul-26 binary and
   concluded that a live subsystem was dead code.** A whole follow-up lane was
   spent disproving it.

**CORRECTION — the "dead bootstrap blob" finding recorded earlier in this file
was WRONG.** `kNodeProcessExtraJS` evaluates to completion; `trace_events` is
live (`typeof globalThis.__mbunTraceEvents` → `object`,
`require('trace_events').createTracing` → `function`). Globals installed at
lines 1371/1399/1504 — well past the alleged abort at ~926 — were present even on
the stale binary, which by itself falsified the reading. The 22 `test-trace`
failures are ordinary semantic gaps (e.g. `test-trace-events-api.js` fails on the
emitted phase character), correctly scoped for a future lane.

The same hazard also *understated* a gain: the repl lanes diffed against a
Jul-26 baseline of 64/106, so repl2 reported +6 when the authoritative gain was
**+14**. A stale baseline corrupts results in both directions.

`node_corpus_runner.py` now refuses both shapes — a superseded build output, and
any binary older than the sources it claims to contain — naming the offending
file and the rebuild command. `--bin auto` selects the newest build.

**Trap for the next dispatcher:** a worktree based on `6dad3b1` carries the OLD
runner, where `--bin auto` is treated as a literal path, silently exec's
`<root>/auto`, and reports every file as `fail` — a very convincing fake
baseline. Base future lane worktrees on the integration branch, or copy the
runner in first.

### Other live findings from this wave

- **`process.argv0` returns the bare string `mbun`** instead of the invoked path
  at `6dad3b1`. It costs any test that respawns through `argv0` (it is why
  `test-repl-array-prototype-tempering.js` fails). Unfixed; cheap and worth a
  dedicated item.
- The builtins blobs contain ~40 inner `try { … } catch (e) {}` blocks, one
  spanning 364 lines (`node_process_extra.cppm` 747–1111). A throw inside one
  silently deletes every statement after it. `d42c5cc` makes whole-partition
  aborts loud but does NOT cover these. Auditing the widest of them is the real
  structural win.

### Selection lesson for wave 41

Single-signature clusters beat subsystem cuts on triage cost — every lane found
its root cause inside the timebox, versus waves 34–39 where lanes burned the box
searching. But a shared signature is not always a shared cause: the runner
cluster's `error: <v>` was just the corpus runner reporting a non-zero exit, and
its 14 files needed 13 different features. **Prefer signatures that name a
contract** (a thrown error type, a missing export) over signatures naming only an
outcome. The two best lanes (repl2 7/12, promise 5/5) were both cases where mbun
already owned a near-complete implementation that was unreachable or stubbed —
that shape is worth hunting deliberately.

Next candidates, ranked: unhandled-rejection cluster (~12 `test-promise*`),
REPL `historyManager` then terminal line-editing (5 left), vm error-stack
decoration (2), `test-runner-worker-id` (single mechanism), `argv0`.

## 2026-07-29 20:20 — wave 40 first five lanes MEASURED (+4 Node, 0 regressions)

Integrated on `agent/corpus-coverage-w40` (based on `origin/rewrite_bun_in_mcpp`
at `6dad3b1`, NOT on the old branch — see the PR note below). Build green;
`check_conflict_markers` and `check_submodule_gitlinks` both clean.

| lane | list | retained | commit |
| --- | --- | --- | --- |
| vm | 0→2 /8 | sandbox writes to realm builtins | `b7d4131` |
| runner | 0→1 /14 | `node:test` tags + `t.plan` validation | `24bc978` |
| compile | 0→1 /15 | compile-cache disable trace | `4c26987` |
| repl | 0/12 | reverted — gate fired | — |
| trace | 0/8 | reverted — gate fired | — |

**Guards, per-file against the wave-39 full run** (not against lane-local
baselines): `test-vm` 52→54/98, `test-runner` 29→30/77, `test-compile` 1→2/22,
`test-module` 15→15/32, `test-require` 16→16/23, `test-esm` 0→0/2. **252 files
checked, +4 gained, 0 lost.**

**A lane's own subtree number is not authority.** The runner lane reported
27→30 including two files "outside its list"; against the wave-39 baseline those
two were *already* passing, so its local baseline was stale and the honest figure
is +1, not +3. Always re-diff a lane's claim against the authoritative full run
before quoting it.

### What wave 40 bought besides the +4

Two of the three zero-yield lanes returned findings worth more than their files:

1. **[FALSIFIED — see the wave-40 final section above. The probes behind this
   came from a stale binary; `trace_events` is live.]** ~~A bootstrap JS blob is
   dead code.~~ `kNodeProcessExtraJS` in
   `modules/jsc/src/builtins/node_process_extra.cppm` contains a substantially
   complete `trace_events` implementation (~lines 936–1110) that never runs:
   evaluation aborts before ~line 926, silently, so `require('trace_events')`
   falls back to a no-op stub. Anything defined after the abort point is missing
   runtime-wide with no error printed — a failure mode that surfaces far away as
   unrelated corpus failures. A dedicated lane is bisecting it.
2. **The repl cluster is a module-identity problem, not a REPL bug.** All 12
   files `require('internal/repl')` under `--expose-internals`; mbun resolves
   that to node's *real* lib via the `internal/*` tsconfig mapping, which then
   dies reaching `internal/deps/acorn` (node vendors deps in `deps/`, not
   `lib/internal/deps/`). Patching that only moves the wall to
   `internal/vm.js` → missing `ContextifyScript` in the contextify binding.
   Measured 0/12 after that patch, so it was reverted. mbun already owns a more
   complete REPL than the signature suggests (`node_repl.cppm`:
   `createInternalRepl` ~1408, `processTopLevelAwait` ~500); serving
   `internal/repl` from it bypasses node's lib chain entirely. A lane is on it.

### Selection lesson for wave 41

Single-signature clusters beat subsystem cuts on *triage* cost — every lane
identified its root cause inside the timebox, versus waves 34–39 where lanes
burned the box searching. But a shared signature is not always a shared cause:
the runner cluster's `error: <v>` turned out to be the corpus runner reporting a
non-zero exit, i.e. an artifact of exit-code bucketing, and its 14 files needed
13 different features. **Prefer signatures that name a contract** (a thrown
error type, a missing export) over signatures that only name an outcome.

## 2026-07-29 20:05 — wave 40 dispatched (IN FLIGHT)

Authoritative baseline for this wave: `target/integration/codex-sprint2-wave39-full-node`
= Node **2821 / 4433 pass (63.64%)**, 951 fail, 89 timeout, 569 skipped, 3 oom.
Bun authority is unchanged at **93/230 green, 5074 pass / 740 fail**.
Build verified green at `a25ef13` (`build_or_die: ok — 607a9025567c80a3`).

**Selection change.** Waves 34–39 cut 12-file lists from *already-mined* subsystems
(tls, http, crypto, fs, net) and yielded 1–6 per 36 files. Wave 40 instead ranks by
`cluster_finder` and takes the five densest **unmined single-signature clusters** —
each lane gets one cause, not one subsystem:

| lane | worktree | branch | files | signature |
| --- | --- | --- | --- | --- |
| compile | `wt1` | `w40/compile` | 15 | `stderr did not match expectation` (compile-cache API) |
| repl | `wt2` | `w40/repl` | 12 | `Node.js v26.<v>` — REPL child dies with a fatal trailer |
| runner | `wt4` | `w40/runner` | 14 | `error: <v>` — `node:test` throws |
| trace | `wt6` | `w40/trace` | 8 | falsy assertion — `trace_events` largely unimplemented |
| vm | `wt7` | `w40/vm` | 8 | strict-equal miss in the new JSC sub-context vm |

Worklists: `target/integration/worklists-w40/<lane>.txt`. Each lane has a 25-minute
timebox, an admission gate of >=2 newly-green at ~8 minutes, mandatory additive
revert of any zero-yield commit, and a subsystem-subtree guard run before reporting.
Parallelism is 5 (was 10); builds serialise through `build_lock.sh`.

**Blocker cleared before dispatch: the disk was 100% full (6.1 GB free).**
`bounded_run.ensure_disk_headroom()` would have refused every measurement, and the
symptom would have read as a runner bug. Cause was accumulated regenerable `target/`
build caches inside *stale* worktrees (`.claude/worktrees/wt1..wt5` = 49 GB,
`.worktrees/codex-wave1-*` = 6 GB); `.claude/worktrees/wt4` alone held 11 GB under
`modules/jsc/target`. Reclaiming only those caches restored **44 GB free**. Active
worktrees, the main `target/`, and the shared `~/.mcpp/bmi` were left untouched.
Use `tools/integration/reclaim_disk.sh` from now on rather than hand-deleting.

## 2026-07-29 accelerated protocol after wave 30

The user correctly identified repeated full-corpus runs as the integration
bottleneck. Effective immediately:

1. Accumulate work for 20–30 minutes before a PR checkpoint; do not push every
   7-minute micro-wave.
2. Combine 6–10 disjoint candidates into one build and run their exact native
   files in parallel.
3. Run the full Node corpus only for Node/CLI/bootstrap/process changes. Run the
   full Bun corpus every 2–3 waves or after an expected cumulative >=50
   assertion reduction.
4. Admission threshold is >=10 failures per 15 minutes or >=2 complete green
   files per 20 minutes. Stop a lane after 5–8 minutes without a credible path.
5. For the Node long tail, generate named disjoint 12-file worklists with
   `make_worklists.py`; each lane skips a file after five minutes and maximizes
   complete green files across its list instead of searching indefinitely for
   a large shared mechanism.

Current authoritative full checkpoint after wave 30: Node 2788/4433 pass
(the only movement is known-flaky weakref); Bun 92/230 green with 5063 pass /
752 fail assertions. Against the first unified Bun checkpoint (4441/1371), this
is +622 pass / -619 fail.

### Wave 31–33 accelerated results

- Wave 31 retained Bun stdin 5/9 -> 11/3 (-6) and hostedGit 0/5 -> 5/0
  (-5 plus one green). The 60-line WebSocket RSV candidate moved only 1/7 ->
  2/6 and was additively reverted.
- Node long-tail wave 32: process +2 green, HTTP +3, child_process +2 stable;
  one transient execfile pass returned to fail and is not counted. Worker +0
  and was fully reverted.
- Node long-tail wave 33: module +4 green, FS +2; HTTP/2 +0 and was fully
  reverted. A zero-yield FS write-buffer commit was also reverted.

The first full Node check invalidated the targeted total: it was 2761/4433,
with 14 fail-to-pass but 41 pass-to-fail HTTP regressions. Reverting the four
HTTP lazy-parser/header-symbol/destroy-error commits restored the complete
398-file HTTP subset from baseline 353 pass to 354 pass. Stable wave-32/33
projection is therefore about +12, pending the corrected full run.

Wave 34 tried 24 files per lane. Crypto delivered 3/24, net/dgram 2/24, and
util/url 0/24. All util code, the zero-yield crypto Hash commit, and the
inaccurate net assessment doc were reverted. Do not repeat the 24-file cut: it
increased static false positives without improving green/minute. Return to 12
high-confidence files and run the complete related subtree before checkpoint.

Corrected full Node authority is now **2805/4433 pass (63.27%)**, +151 against
the PR starting point 2654. The one TLS socket pass-to-timeout was traced to
preconnect flush changes and its exact guard is green again; an unexpected
worker pass failed on same-binary rerun and is treated as a flake.

Wave 35 retained exactly three new Node greens: ECDH setPublicKey DEP0031,
module.parent DEP0144, and legacy require-mjs. The first module implementation
leaked a global and regressed pending-deprecation crypto tests; it was changed
to closure-private state. A net no-halfopen candidate regressed an existing
local-address test and was fully reverted. Complete crypto/module/require
subtrees show only the three intended fail-to-pass transitions. The subsequent
full run is authoritative at **2808/4433 pass (63.34%)**: the three intended
greens plus recovery of the prior TLS socket timeout, offset by
`test-worker-terminate-source-map.js` changing pass to fail. That worker file
fails on three consecutive same-binary reruns and must not be counted as green.

Wave 36 returned to three 12-file high-confidence lists. Static expectation was
10+, but exact native runs produced **6/36 newly green**: process default export,
env deprecation, process ref/unref, and three child-process contracts
(prototype tampering, spawn error, stdin). Three zero-yield process candidates
(hrtime, exit-code validation, getBuiltinModule type validation), one
compile-failing dlopen candidate, and an inaccurate assessment document were
additively reverted. Complete `test-process*` (96 files), `test-child*` (111),
and `test-cluster*` (83) subtrees show exactly the six fail-to-pass transitions
and no pass-to-nonpass transition. The required full Node run landed at
**2812/4433 (63.43%)**, net +4: the six intended greens plus one watch-mode
timeout recovery, offset by three unrelated standalone-stable failures. Two are
runner/PATH-sensitive child-spawn tests (`spawn mbun ENOENT`) and one is the
known nondeterministic weakref GC test; none exercises the changed wave-36
contracts. `wave_report.py` nevertheless counts the conservative global net,
**11 green/hour over 0.35h**, not the targeted +6.

Wave 37 tested errors, HTTP/2, and TLS/PFX across another 36-file cut. Static
prediction was six; exact native yield was only **1/36**:
`test-tls-server-capture-rejection.js`. The errors, HTTP/2, and PFX candidates
were additively reverted. The retained 10-line TLS change rebuilt successfully;
complete `test-tls*` (217 files) and `test-https*` (63 files) guards show the
single intended fail-to-pass and no pass-to-nonpass transition (one
`test-tls-fast-writing` timeout became an OOM, neither is a green regression).
Do not prioritize static error-message, PFX, or one-file HTTP/2 predictions in
the next cut. Wave 38 is module, fs, and net/dgram, again 12 files per lane.

Wave 38 stopped the module lane read-only when it found no >=2-green mechanism
and redirected it to streams. Across stream, net/dgram, and fs, exact yield was
**5/36**: three stream files shared one EventEmitter `removeListener` bug, and
two net files shared explicit IPv6 custom-lookup handling. The fs FileHandle and
net write-queue candidates yielded zero and were reverted. Complete stream
(249), net (150), and dgram (76) subtrees show exactly five fail-to-pass and no
pass-to-nonpass transition. Observed end-to-end rate was about **20 green/hour**.

Wave 39 selected the largest concrete failure signature rather than another
subsystem-first cut: 20 files expected `ERR_INVALID_ARG_TYPE`, split between
async/events, crypto, and misc runtime. Exact yield was **3/20**:
AsyncLocalStorage.bind, zlib invalid synchronous input, and V8 heap-profile
options. The zero-yield crypto candidate was reverted. Complete async (56),
zlib (62), and V8 (23) subtrees show exactly those three fail-to-pass transitions
and no pass-to-nonpass. Node projection from the wave-36 full authority is now
**2821/4433 (63.64%)**, +167 from the PR start, pending the next full Node run.

The overdue full Bun checkpoint also completed on the same retained tree:
**93/230 green, 5074 pass / 740 fail assertions**. Relative to wave 30,
hostedGitInfo became green and process.stdin moved 5/9 -> 11/3. There is no
green regression; one third-party next-auth file moved to blocked-external.
This replaces the prior Bun projection with authoritative full-corpus evidence.

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

## 2026-07-29 five-hour sprint protocol

The 10:20–10:55 wave produced only 2 newly green named Node files in 35
minutes with three implementation lanes: **3.4 files/hour**. Do not resume the
per-assertion loop. BroadcastChannel and `test-runner-cli.js` advanced through
many assertions but remained red, so their progress is not coverage yield.

For the next five hours:

1. Rank work by expected newly green files divided by wall-clock hours. Accept
   tasks forecast at 10+ files or 5+ files/hour; abandon or split a lane after
   45 minutes without evidence of batch yield.
2. Agents implement one shared root cause on their persistent lane and do only
   a cheap reproducer. Integration owns the one combined build, targeted
   acceptance, related-subtree guard, checkpoint commit, push, and PR comment.
3. Every PR checkpoint comment must report estimate vs actual, wall-clock
   time, files/hour, and pass→non-pass regressions. Never count assertion
   advancement as a green file.
4. Do not repoint a warmed lane for unrelated integration commits. That caused
   near-full rebuilds behind the global lock and serialized the previous wave.
5. Run full Node and Bun corpora only at a pushed PR checkpoint. The interrupted
   runs `codex-wave3-full-node` (3690/4433) and `codex-wave3-full-bun`
   (395/1902) are partial journals, not baselines.

The published gap is Node 1779 plus Bun 1034 = 2813 files, requiring 562.6
files/hour for five-hour completion. The next dispatch is therefore broad
shared gaps: trace-events (20+ expected), FastUtf8Stream (14 expected), and
compile-cache (14+ expected). Defer the one-file cross-process
BroadcastChannel transport until its reuse count justifies the cost.

`node_corpus_runner.py` now launches from the upstream checkout root
(`compat/node`). Old and new full-run counts are different measurement
contracts and must not be subtracted. Establish the new baseline at the next
pushed checkpoint.

### Full checkpoint after wave 26

The pushed wave-26 tree completed both current runner measurements:

- Node: **2787 / 4433 pass (62.87%)**, 977 fail, 97 timeout, 3 OOM, and
  569 skip. This is +133 against the published 2654-pass starting point, with
  1646 current non-pass files.
- Bun executable discovery: **89 / 230 green**, 123 test-failure, 1 timeout,
  4 blocked-external, 11 all-skipped, 1 no-tests, and 1 ahead-of-reference.
Assertion totals are 4441 pass / 1371 fail.

Named causal wave accounting totals +147 while full-corpus net movement is
+133. Treat the 14-file difference as proof that scoped guards are not a
substitute for full checkpoint de-duplication/regression measurement. For Bun,
rank by failed assertions per wall-clock rather than files: JSON5 (205),
JSONL (171), and WPT Streams (120) are the current top three.

### Sprint wave 27 measured checkpoint (14:57–15:15)

The first assertion-ranked Bun wave reduced **438 failed assertions/tests in
18 minutes** (about **1460/hour**) and made `Bun.inspect.table` fully green:

- JSONL -166, JSON5 -143, Bun.inspect.table -35, WPT Streams -28;
- TOML -23, REPL -19, GFM -10, crypto -7, URLPattern -4, ALS -3.

All ten targets moved positively, but only Bun.inspect.table is a newly green
file; do not convert the other assertion deltas into file coverage. JSONL's
first candidate OOM was fixed before acceptance by restoring the native
allocation guard. The exact build, conflict-marker check, submodule-gitlink
check, and `git diff --check` pass.

Continue dispatching by failed assertions per minute. Prefer one missing API or
entrypoint that owns a large homogeneous cluster; stop investing in dispersed
tails such as the remaining URLPattern failures. Wave 28 is analyzing the
remaining TOML 25, crypto 17, and REPL 98 failures. Run complete Node and Bun
corpora only after the wave-27 PR push, then use that result to detect global
regressions and re-rank.

The pushed-tree full measurements are now complete:

- Node **2788/4433 pass**, 984 fail, 88 timeout, 4 OOM, 569 skip. This is +1
  pass with zero pass-to-non-pass transitions; seven former timeouts now fail
  and one now OOMs.
- Bun **91/230 green**, 122 test-failure, 3 blocked-external, 11 all-skipped,
  1 load-error, 1 no-tests, 1 ahead-of-reference. Assertions are
  **4880 pass / 933 fail**, exactly +439/-438 globally.

The extra Bun green beyond Bun.inspect.table is
`native-source-onclose-leak.test.ts`, a WPT Streams side effect. Use these
directories as the next frozen comparison:
`target/integration/codex-sprint2-wave27-full-node` and
`target/integration/codex-sprint2-wave27-full-bun`.

### Sprint wave 28 measured checkpoint (15:25–15:32)

One build reduced another **63 Bun failures in 7 minutes** (~540/hour):

- REPL byte stdin forwarding: 19/98 -> 69/48, -50 failures;
- TOML parse input and safe-integer boundaries: 58/25 -> 71/12, -13.

Both files remain red and add zero complete-file greens. Stop funding their
dispersed tails. Wave 29 is assigned to JSON5's remaining 62, WPT Streams'
remaining 92, and bundler DCE's 53; each lane must find a homogeneous >=20
cluster or stop within ten minutes.

Wave-28 full proof: Bun is 91/230 green at 4943 pass / 872 fail assertions.
The targeted +63/-63 is offset globally by two newly measured failures when
one Svelte integration moved from blocked-external to test-failure. Node stays
2788/4433 pass; weakref and watch-mode exchanged fail/pass versus timeout, and
a same-binary rerun showed both are unstable rather than stable coverage.

### Sprint wave 29 measured checkpoint

- JSON5 error taxonomy: 259/62 -> 304/17, -45 failures;
- WPT byte-stream tee branches: 1083/92 -> 1097/78, -14 failures.

Total is -59. The WPT estimate (30+) overpredicted actual 14, so recluster the
remaining 78 from the fresh log instead of continuing the same hypothesis.
DCE, WebView, Inspector profiler, and node:test were stopped statically because
they require missing multi-stage engines/backends or split across independent
mechanisms; no stubs were accepted.

Wave-29 full proof: Bun remains 91/230 green at 5001 pass / 814 fail
assertions, globally +58/-58 because ws-proxy regressed one assertion and
reproduced at the lower count in a same-binary guard. Node is 2787/4433 pass;
weakref returned to fail after already failing in the previous same-binary
guard, so no stable Node gain/loss is attributed to this source wave.

### Sprint wave 30 measured checkpoint

- GFM tagFilter: 30/32 -> 47/15, -17;
- JSX text entity lowering/direct-readable-stream: 254/15 -> 268/1, -14;
- cron invalid dates: 12/12 -> 24/0, -12 and one complete green;
- bunfig/CLI preload merge: 6/12 -> 16/2, -10;
- test discovery path-ignore: 1/9 -> 9/1, -8.

Total is -61 and one complete green. Wave 31 is assigned to the fresh GFM
remaining 15, shared WPT/Undici HTTP/2 failures, and import-attributes; retain
only homogeneous clusters meeting their per-lane thresholds.

### Sprint wave 1 measured checkpoint (10:55–11:15)

One combined build after three static implementation lanes produced 29 newly
green files in 20.8 minutes: **83.5 files/hour**, with zero pass regressions in
the named acceptance sets.

- domain abort 10/10;
- trace-events 10/29 (10 new);
- VM module request/link/TLA 5/7;
- FastUtf8Stream 2/14;
- compile-cache 1/22;
- Bun CSS minifier bridge 1/10;
- expose_gc 0/5.

Node gained 28 and Bun gained 1. Do not extrapolate the scoped zero-regression
result to the full corpora. The trace implementation is intentionally honest:
the integration review removed synthetic V8/bootstrap/async event records and
kept only real category state, API/internal-binding calls, metadata and file
writer behavior.

Re-rank before dispatch. Do not keep funding the original FastUtf8/CSS/GC
groups as if their first missing symbol were their only cause. Compile-cache
engine tests need real loader bytecode persistence and exceed one sprint lane.
Prefer another shared fatal/state/API endpoint with an estimated 5+ files/hour.

### Sprint wave 2 measured checkpoint (11:15–11:30)

The second combined checkpoint gained 8 Node files in 15.7 minutes:
**30.6 files/hour**, with zero green-to-non-green regressions in the named
acceptance sets.

- POSIX process credentials: estimated 4, actual 4/4. The implementation uses
  real libc credential syscalls and passwd/group lookup.
- FastUtf8Stream drain lifecycle: estimated 5, actual 1 additional file across
  the 14-file group. The first integration run regressed periodic flush because
  destroy swallowed a requested flush callback; integration fixed that before
  accepting the wave, leaving zero scoped regressions.
- Web Compression Streams: estimated 6, actual 3/6, backed by the real
  incremental zlib Transform and BufferSource Web adapter.

The active next wave is async_hooks lifecycle (static estimate at least 5) plus
fresh Bun HTTP/serve/TLS gate-log clustering. Continue to reject first-error
movement as yield and stop any lane that cannot identify a shared >=5-file
mechanism inside ten minutes.

### Sprint wave 3 measured checkpoint (11:30–11:37)

The async_hooks lifecycle wave gained 4 files across the complete 55-file
`test-async-*` acceptance set in 7 minutes: **34.3 files/hour**, with zero
green-to-non-green regressions.

The first integration run was +4/-1 because global `triggerAsyncId()` still
returned zero inside recursively nested `AsyncResource.runInAsyncScope()`.
Integration fixed it to read the active resource, rebuilt, and reran all 55
files before accepting the wave. JSC-native await allocation, GC destroy, and
deep native promise timing remain engine seams; do not claim the whole async
group is compatible.

### Sprint wave 4 measured checkpoint (11:37–11:51)

Three short shared roots gained 7 files in 14.5 minutes: **29.0 files/hour**,
with zero green-to-non-green regressions in the 9-file acceptance set.

- child_process maxBuffer: estimated 4, actual 4/4;
- RSA-PSS key restrictions/details: estimated 3, actual 2/3;
- HTTP/2 response splitting sanitation: estimated 1, actual 1/1.

The maxBuffer guard also reran the already-green spawnSync file. The remaining
RSA-PSS file moved to an independent sign-padding failure and is not counted as
key-details yield. Integration resolved the stale-lane conflict by preserving
both the existing RSA public exponent parameter and the new PSS restriction
parameters in one 14-argument native ABI.

### Sprint wave 5 measured checkpoint (11:51–11:55)

DSA JWK rejection plus the generic HTTP/2 native submit-error adapter made all
4 target files green in 4 minutes: **60 files/hour**.

- DSA JWK unsupported output: 1/1;
- HTTP/2 `info`/`respond` negative nghttp2 errno to stream-level
  `NghttpError` and existing destroy/RST flow: 3/3.

All four transitions are complete-file fail/timeout to pass, not first-error
movement.

### Sprint wave 6 measured checkpoint (11:55–12:06)

Four short contract groups gained 7 complete files in 11.5 minutes:
**36.5 files/hour**, with zero named-set regressions.

- `urlToHttpOptions` shape plus invalid argument: 1;
- HTTP/2 file response HEADERS then fd I/O error to stream/RST: 1;
- EventEmitter once return/re-entrancy semantics: 2;
- MaxListeners process warning routing and message limit: 3.

Two zero-yield attempts, EC `paramEncoding` and URL.canParse required-argument
validation, were additively reverted after runtime checks proved that they only
moved the first error. They are not part of the accepted checkpoint.

### Sprint wave 7 measured checkpoint (12:06–12:22)

This checkpoint gained 7 files in 15.8 minutes: **26.6 files/hour**, with zero
regressions in the named/related sets.

- EventEmitter direct single-listener storage and array promotion/demotion: +2.
  The full 26-file `test-event-emitter-*` guard was 23 pass / 3 fail, compared
  with the old gate as 7 gains, 16 retained passes, and zero regressions (five
  of those gains were already published in wave 6).
- protected AbortSignal cleanup listeners: estimated 2, actual +1; the other
  file reached an independent error-code failure.
- legacy URL parse DEP0169 plus URLSearchParams inspect/brand/iterator/callback/
  nested-query contracts: 4/4.

### Sprint wave 8 measured checkpoint (12:22–12:29)

Five static candidates forecast six complete files; the current post-revert
tree gained 3 files in 7 minutes: **25.7 files/hour**, with zero
green-to-non-green regressions in the six-file acceptance set.

- retained URL properties in HTTP request options: estimated 1, actual 1/1;
- defer ClientRequest timeout until connect: estimated 2, actual 1/2; the
  remaining file needs a distinct `_idleTimeout` shape contract;
- Web Streams queuing strategy accessor brands: estimated 1, actual 1/1;
- aborted-request destruction and Encoding Streams state validation: estimated
  1 each, actual 0 each. Both zero-yield changes were additively reverted.

After those reverts integration rebuilt the exact checkpoint tree and reran all
six targets: 3 pass, 3 retained failures, zero regressions. Continue accepting
only complete-file transitions; an implementation that advances to a later
assertion is reverted at the same checkpoint.

### Sprint wave 9 measured checkpoint (12:29–12:35)

Two static lanes forecast seven complete files; the combined checkpoint gained
4 in 6 minutes: **40 files/hour**, with zero green-to-non-green regressions.

- net.Server accepted-handle admission for `blockList` and `maxConnections`:
  estimated 4, actual 4/4. The two old failures and two old timeouts all pass;
  rejected peers close without reaching `connection` and emit `drop` metadata.
- fs promises temporary FileHandle close/error aggregation: estimated 3,
  actual 0/3. Exposed-internal tests patch a different FileHandle identity from
  the public promise path. Correctness requires unifying the internal/public
  routing rather than adding another wrapper, so the change was additively
  reverted inside the checkpoint.

Integration rebuilt after the fs revert and reran the four retained targets:
4/4 pass. Conflict-marker, gitlink, and diff guards are clean. Keep short lanes
on shared runtime endpoints; route cross-identity architecture work out of the
five-hour path unless its measured reuse count rises substantially.

### Sprint wave 10 measured checkpoint (12:35–12:44)

The experimental stream/iter FileHandle adapters forecast three complete
files and delivered **3/3 in 9.5 minutes = 18.9 files/hour**:

- `FileHandle.pull()` and `pullSync()` implement position, limit, chunk,
  transform, locking, abort, and auto-close contracts;
- `FileHandle.writer()` implements async/sync write and writev, position/limit,
  failure/close/dispose, and handle locking;
- integration fixed two sequential writer contracts exposed by the combined
  gate: `endSync()` returns `-1` during an async write, while lock errors remain
  ordinary Error and writes after writer closure reject with TypeError.

The complete 19-file `test-fs-promises-file-handle-*` guard finished at 15 pass
and 4 retained failures: +3, zero frozen-gate regressions. A first `--jobs 8`
guard produced one transient JSC rope-string SIGSEGV in an otherwise
already-green dispose file; it passed immediately alone, and the complete guard
rerun at `--jobs 4` had zero regressions. Record the transient honestly, but do
not attribute an unreproduced concurrent crash to the adapter.

### Sprint wave 11 measured checkpoint (12:44–12:49)

Three cleanup packages forecast nine complete files and delivered **+8 in
5 minutes = 96 files/hour**:

- URL object/file helpers: estimated 3, actual 2. Blob argument errors and raw
  byte/malformed UTF-8 file URL conversion pass; `test-data-url` only advanced
  to an independent MIME percent-token parser failure and is not counted.
- EventEmitter error contracts: estimated 3, actual 3/3.
- stable Immediate/Timeout facade identity, callback `this`, dispose/registry
  state: estimated 3, actual 3/3.

The complete 26-file EventEmitter guard is 26/26. The 57-file `test-timers-*`
guard is 46 pass and 11 retained failures. After removing gains already
published in earlier checkpoints, this wave is exactly +8 with zero
green-to-non-green regressions. Continue treating first-error movement as zero.

### Sprint wave 12 measured checkpoint (12:49–12:58)

Console, Buffer, and crypto-warning packages forecast nine files and delivered
**+7 in 9.5 minutes = 44.2 files/hour**:

- Buffer detached backing-store and null-prototype input contracts: 3/3;
- SHAKE default-output `DEP0198` and non-extractable CryptoKey `DEP0204`
  warnings: 3/3;
- mutable global-console stdout/stderr sinks: 1/3. Diagnostics-channel registry
  identity and revoked-Proxy `util.inspect(showProxy)` are independent roots.
  Their zero-file speculative code was removed by an additive cleanup commit.

The 68-file `test-buffer-*` guard is 52 pass, 14 retained failures, and 2
skips. The 21-file `test-console-*` guard is 16 pass, 4 retained failures, and
1 timeout. Both have zero frozen-gate regressions; all three crypto targets
pass.

### Sprint wave 13 measured checkpoint (12:58–13:09)

DNS, assert, and stream async-context packages forecast eight files and
delivered **+5 in 11 minutes = 27.3 files/hour**:

- Resolver server/channel state: estimated 3, actual 2. Integration added
  rrtype validation, but `test-dns.js` then reached an independent lookup
  options error-code mismatch and remains uncounted.
- assert fail/ifError/async: estimated 3, actual 2. Adding the missing
  AssertionError stack name/message prefix closed fail; async remains at a
  separate generatedMessage contract.
- stream finished AsyncResource/ALS binding: estimated 2, actual 1. The other
  file observes a separate exposed-internal async-context identity.

Guards: 13 assert files are 5 pass, 7 retained failures, 1 OOM; 28 DNS files
are 17 pass, 7 retained failures, 4 timeouts; the four stream-finished files
are 2 pass and 2 retained failures. All three groups have zero regressions.

### Sprint wave 14 measured checkpoint (13:09–13:17)

HTTP client, module/require, readline, Abort timeout, and BroadcastChannel
packages forecast 13 files and strictly delivered **+9 in 8 minutes = 67.5
files/hour**:

- HTTP client lifecycle/parser/globalAgent: 3/3 after integration normalized
  signal reasons to `AbortError.code = ABORT_ERR`;
- module/require validation: 3/3 targets plus `test-module-loading-error`, +4;
- weak unref Abort timeout: estimated 2, actual 1; a weak listener record still
  retains the signal;
- BroadcastChannel depth inspect: +1;
- four readline targets self-skipped on a dumb terminal. They exit zero but
  count as **zero compatibility gains**.

Guards: HTTP client 63/68 pass; module 11 pass, 18 fail, 3 skip; require 13
pass, 9 fail, 1 skip; abort 1/7 pass; readline 10 pass, 3 fail, 8 skip. Every
guard has zero green-to-non-green regressions.

### Sprint wave 26 measured checkpoint (14:44–14:48)

IPC UTF-8 framing/backpressure plus exec maxBuffer chunk typing forecast four
files and delivered **+2 in 4 minutes = 30 files/hour**:

- byte-framed IPC decoding made send-utf8 green; send backpressure remains red;
- preserving string/Buffer chunk types made execFile maxBuffer green while the
  already-published exec-maxbuf file stayed green;
- both published exec encoding files also remained green in the combined gate.

The exact six-file gate is 5 pass / 1 retained failure. Named incremental yield
is two with zero green-to-non-green regressions. Structural guards are clean.

### Sprint wave 25 measured checkpoint (14:38–14:44)

ExecFile result/promisify contracts, option/env normalization, and IPC stdio
validation forecast six files and delivered **+3 in 6 minutes = 30 files/hour**:

- promisified exec/execFile exposes `.child` and retains error stdout/stderr:
  +1. Integration limited DEP0190 to one warning, but execFile still fails on an
  independent child exit-code gap and is not counted;
- envPairs precedence plus multiple-IPC rejection: +2/2;
- both options/env prototype targets remained red, so that candidate was
  additively reverted.

The exact retained four-file gate is 3 pass / 1 retained failure, all three
greens are frozen fail-to-pass, with zero green-to-non-green regressions.
Structural guards are clean.

### Sprint wave 24 measured checkpoint (14:22–14:38)

After rejecting zero-yield TLS/TextDecoder candidates, exec encoding and
promisified AbortSignal work delivered **+3 in 16 minutes = 11.25 files/hour**:

- valid exec encodings now decode exposed stream data, while invalid and
  explicitly undefined/null/buffer encodings return Buffer. Integration added
  omitted-vs-explicit-undefined handling; both named encoding files pass;
- exec/execFile custom promisifiers synchronously validate AbortSignal before
  Promise construction: +1/2. The exec case moved fail-to-timeout and remains
  zero yield;
- TLS/HTTPS targets were already frozen-green and both TextDecoder failures
  remained red, so all three zero-increment commits were additively reverted.

The 14-file child exec guard is 8 pass, 2 retained failures, 3 timeouts, and 1
skip. Named yield is three with zero green-to-non-green regressions. An old
exec-maxbuf green belongs to the earlier published maxBuffer wave and is not
counted again. Structural guards are clean.

### Sprint wave 23 measured checkpoint (14:15–14:22)

StringDecoder validation, DNS resolver channel state, and concatenated gzip
forecast at least five files and delivered **+3 in 7 minutes = 25.7 files/hour**:

- observable Resolver ChannelWrap routing: +2/2;
- concatenated/trailing gzip handling: +1/3. Its first integration build exposed
  a malformed inflate loop; integration corrected the loop before runtime
  acceptance;
- both StringDecoder related files were already frozen-green, so that
  zero-increment commit was additively reverted.

The 13-file DNS/zlib guard is 9 pass, 3 retained failures, and 1 timeout:
three frozen fail-to-pass transitions and zero green-to-non-green regressions.
Structural guards are clean.

### Sprint wave 22 measured checkpoint (14:11–14:15)

V8 transferArrayBuffer state plus DNS lookup boolean validation forecast four
files and delivered **+1 in 4 minutes = 15 files/hour**:

- shared `all`/`verbatim` validation made the promise deprecated-options DNS
  file green; the callback file still has an independent failure;
- V8 serdes remained red while two existing serialization guards stayed green,
  so the zero-yield V8 commit was additively reverted.

The five-file DNS lookup guard is 4 pass / 1 retained failure: one frozen
fail-to-pass and zero green-to-non-green regressions. Structural guards are clean.

### Sprint wave 21 measured checkpoint (14:03–14:11)

VM compile validation plus Worker/MessagePort async-hook lifecycle forecast
four to five files and delivered **+3 in 8 minutes = 22.5 files/hour**:

- WORKER/MESSAGEPORT resources now use the existing active hook registry;
  integration corrected callback `this` to the returned hook controller and
  delayed MessagePort ref clearing to the close event. All three hasRef files
  pass;
- both VM failure targets remained red, while two prior validation guards stayed
  green. The zero-yield VM commit was additively reverted.

The exact rebuilt Worker gate is 3/3, all frozen fail-to-pass, with zero
green-to-non-green regressions. Structural guards are clean.

### Sprint wave 20 measured checkpoint (13:55–14:03)

Hash/Hmac output validation, child maxBuffer, module/require validation, and
Worker entry protocol delivered **+2 in 8 minutes = 15 files/hour**:

- string URL and URL+eval Worker validation: +2/2 after integration corrected
  the exact `file://` guidance text;
- Hash/Hmac remained whole-file red and was additively reverted;
- child maxBuffer and module/require candidates were semantically identical to
  wave 4 and wave 14 code respectively, so conflict review skipped them without
  duplicate commits or gains.

Both Worker targets changed frozen fail-to-pass and pass 2/2 on the exact rebuilt
tree. There are zero green-to-non-green regressions; structural guards are clean.

### Sprint wave 19 measured checkpoint (13:50–13:55)

DH/ECDH uninitialized state plus terminal net write errors forecast four files;
strict acceptance delivered **+1 in 5 minutes = 12 files/hour**:

- destroyed-socket write error contract: +1/2; the other target moved from fail
  to timeout and remains zero yield;
- all five related DH/ECDH failures remained non-green, so the crypto commit was
  additively reverted.

The ten-file net write/socket-destroy guard finished at 9 pass and 1 timeout.
Only the named file changed fail-to-pass; every frozen pass remained pass.
Structural guards are clean.

### Sprint wave 18 measured checkpoint (13:45–13:50)

Keygen, sign/verify, lenient HTTP parsing, timer promisify hooks, and net
auto-select defaults forecast roughly 13 complete files; strict causal
accounting delivered **+2 in 5 minutes = 24 files/hour**:

- per-stream insecure HTTP parser: +1/2;
- auto-select attempt-timeout CLI default: +1/3;
- keygen/sign and timer candidates remained whole-file red and were additively
  reverted. Four green RSA/keygen files in the target batch came from earlier
  published RSA work and are not attributed to this wave.

The eight-file retained HTTP/net guard has 3 pass and 5 retained failures:
two frozen fail-to-pass transitions and zero green-to-non-green regressions.
Conflict-marker, gitlink, and diff guards are clean.

### Sprint wave 17 measured checkpoint (13:38–13:45)

Seven static candidates forecast roughly 12–13 complete files; strict
acceptance delivered **+5 in 7 minutes = 42.9 files/hour**:

- HTTP Agent limit/timeout validation: +2;
- terminal parser detachment: +1/2;
- CompressionStream BufferSource validation: +2 across the related guard;
- crypto random, HTTP pipeline clocks, KeyObject export, and util promisify:
  zero complete files, all additively reverted. A follow-up null-chunk error-code
  patch also remained whole-file zero-yield and was reverted.

The 36-file HTTP agent/parser/compression guard finished at 25 pass and 11
retained failures: five frozen fail-to-pass transitions and zero
green-to-non-green regressions. Conflict-marker, gitlink, and diff guards are
clean.

### Sprint wave 16 measured checkpoint (13:26–13:38)

The async-hooks timer-bootstrap and net pre-connect write package forecast four
named files and delivered **+8 complete files in 12 minutes = 40 files/hour**:

- rebinding timer facades after bootstrap delivered the two named async-hooks
  files plus four same-root lifecycle files, for +6 total;
- retaining pending state through the public connect event and reporting
  backpressure for every pre-connect write delivered +2 net files;
- the stale RSA-PSS candidate was semantically absorbed by the current
  14-argument ABI/restriction implementation, so integration skipped its
  conflicts and counted no duplicate gain.

The 60-file related guard finished at 33 pass, 24 retained failures, and 3
timeouts. Relative to the frozen gate it has eight fail-to-pass transitions and
zero green-to-non-green regressions. The exact four-file target gate is 4/4.

### Sprint wave 15 measured checkpoint (13:17–13:26)

CLI syntax-check, diagnostics module tracing, Buffer DEP0005, and OS internal
contracts forecast 13 files and delivered **+9 in 9 minutes = 60 files/hour**:

- CLI `--check` dispatch: estimated 4, actual 2; two remain at separate stderr
  text/option-dispatch contracts;
- diagnostics module require/import tracing: 4/4;
- Buffer legacy constructor warning: estimated 2, actual 1; default-vs-
  node_modules callsite policy is separate from pending deprecation;
- OS signal freeze, checked binding, userInfo getter: estimated 3, actual 2;
  the third target was already green in the frozen baseline.

Guards: CLI 7/18 pass; diagnostics 64/67 pass; Buffer 54 pass, 12 fail, 2
skip; OS 6/7 pass. All have zero regressions. Additional old-frozen gains
visible in the broad guards are not attributed to this wave.

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

- **Integration branch**: `agent/corpus-coverage-w40`, pushed to origin. PR **35**
  targets `rewrite_bun_in_mcpp`, accounting cumulative from round 1 (2654 →).
  121 commits ahead as of wave 55 (`27b83c6`).
- **Integration worktree**: the main checkout. `wt1`, `wt2`, `wt4`, `wt6`–`wt11`
  are lane worktrees; reclaim their `target/` with `reclaim_disk.sh` when disk
  tightens (it went to 100% once and every measurement failed looking like a
  runner bug).
- **Last authoritative measurement**: node **~2926 / 4433** (66.0%), bun
  **892+ / 1902**. Zero regressions across ~3000 measured guard files.
- **Subtree movement from the wave-39 baseline**: `test-crypto` 86→101,
  `test-vm` 52→69, `test-http2` 213→223, `test-stream` 233→237, bun `shell/`
  20→27.
- **Stale-binary class is now closed in the tooling**: `node_corpus_runner.py`
  takes `--bin auto` and refuses a superseded or source-older binary. The bun
  runner has **no** `--bin auto` and needs `--cwd compat/bun`.

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
