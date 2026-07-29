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
