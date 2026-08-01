# W41 Linux corpus coverage record

## Scope

This checkpoint continues PR #36 against `rewrite_bun_in_mcpp`. It uses exact
failure-file lists, bounded runners, and one coordinator build. It does not
claim a new full-corpus score.

## Baseline and planner

| corpus | full baseline | actionable files |
| --- | ---: | ---: |
| Node | 3134/4433 green (70.7%) | 425 |
| Bun | 1015/1902 green (53.4%) | 170 |

Commit `00b1fcc` makes throughput totals ignore rows whose `minutes` or
`delivered` fields are `PENDING`. The fixture now fails before the fix and the
planner self-test passes after it. The resource guard planned five lanes under
the observed Linux limits; the coordinator owns the only root build.

## Probe ledger

| wave | lane | selected files | result | strategy |
| --- | --- | ---: | --- | --- |
| 1 | Node crypto | 24 | 0 green, 24 fail | park; native/PQC/FIPS/Argon2 walls |
| 1 | Node VM | 19 | 0 green, 19 fail | retain only isolated candidates |
| 1 | Node WebCrypto | 19 | 0 green, 19 fail | park; unsupported algorithms and WebIDL gaps |
| 1 | Bun third-party | 25 | 0 green, 1280 pass / 134 fail assertions | park; optional native/TLS/external runtime |
| 1 | Bun CLI/run | 17 | 0 green, 19 pass / 258 fail assertions | park; package/FUSE/bun:bundle gaps |
| 2 | Node test-runner | 30 | 0 green, 30 fail | keep; snapshot surface is the clearest seam |
| 2 | Node test-util | 12 | 0 green, 12 fail | keep only low-risk isolated APIs |
| 2 | Node test-v8 | 11 | 0 green, 11 fail | park; profiler/queryObjects/startup snapshot ownership |
| W83 | Bun/Node path | 5 Bun + 1 Node guard | pre 4/5 Bun green; post 5/5 Bun green; Node 1/1 | issue #50; dialect-only error-text fix |
| W84 | Bun Node-path continuation | 5 | 5/5 green; 88 passed / 0 failed / 89 ran | retain as green path coverage; no source owner |
| W85 | Bun os/string_decoder | 5 | pre 3 green + 1 skipped; post 4 green + 1 skipped; 149 passed / 0 failed / 150 ran | issue #51; dialect-aware Buffer ceiling |
| W86 | Bun Buffer completion guards | 5 | 4/5 green; 25 passed / 6 failed / 31 ran | park concat multi-owner; preserve four green guards |
| W87 | Bun Node-fs leaves | 5 | 5/5 green; 46 passed / 0 failed / 70 ran / 92 expects | fresh confirmation; no source owner |
| W88 | Bun Node-fs directory/Stats leaves | 5 | 5/5 green; 52 passed / 0 failed / 55 ran / 138 expects | fresh confirmation of narrow fs leaves; no source owner |
| W89 | Bun Node-inspector probe | 5 | 4/5 green; 34 passed / 27 failed / 64 ran / 131 expects | park inspector-profiler behind missing inspector/profiler subsystem |
| W90 | Bun Node-fs/child_process leaves | 5 | corrected probe: 4/5 green; 39 passed / 12 failed / 56 ran / 107 expects | retain four green leaves; park fs/promises multi-owner failures |
| W91 | Bun Node-net leaves | 5 | 3/5 green; 5 passed / 1 failed / 7 ran / 15 expects; one no-tests stress fixture | retain three green net leaves; park autoSelectFamily liveness |
| W92 | Bun/Node fs/promises AbortError owner | 5 Bun + 3 Node guards | Bun target 24 passed / 5 failed / 34 ran; 4 Bun guards green; Node 2/3 pass | issue #52; focused message fix landed, remaining fs/promises owners parked |
| W93 | Node fs/promises FileHandle leaves | 5 | 5/5 files pass; no build | retain FileHandle coverage after W92 fix; no source owner |
| W94 | Bun HTTP/2/Worker staged leaves | 5 | pre-fix 4/5 green; 7 passed / 1 failed / 8 ran / 6 expects | issue #53; isolate reserved-push DATA state |
| W95 | Bun HTTP/2/Worker staged regression | 5 | post-fix 5/5 green; 8 passed / 0 failed / 8 ran / 8 expects | issue #53 landed; retain all five guards |
| W96 | Node HTTP/2 RST lifecycle | 5 Node + 5 Bun guards | pre-fix Node 4/5 pass; post-fix Node 5/5 pass; Bun 5/5 green, 8 passed / 0 failed / 8 ran / 8 expects | issue #54; align readable end and non-zero peer-RST error delivery |
| W97 | Node HTTP/2 connect-abort teardown | 5 Node + 5 Node/Bun regression guards | pre-fix Node 4/5 pass; post-fix 5/5 pass; W96 Node 5/5 pass; W95 Bun 5/5 green, 8 passed / 0 failed / 8 ran / 8 expects | issue #55; preserve session AbortError while canceling streams with `ERR_HTTP2_STREAM_CANCEL` |
| W98 | Bun standard-module/API leaf probe | 5 | 4/5 files green; 161 passed / 1 ahead-of-reference / 165 ran / 100536 expects | retain four green leaves; classify `require`'s passing `test.failing` case as ahead-of-reference, no source owner |
| W99 | Bun fs/streams/spawn/DNS/URL leaves | 5 + DNS isolated rerun | initial 4/5 green; 108 passed / 1 failed / 109 ran / 355 expects; DNS isolated 1/1 green with 69/69 | retain four stable leaves; DNS public-answer variance is external, no source owner |
| W100 | Node fs/promises FileHandle leaves | 5 | 5/5 files pass; no build | retain five green leaves; no source owner |
| W101 | Node module loader / CLI entry leaves | 5 | pre-fix 3/5 pass + 1 skipped + 1 fail; post-fix 4/5 pass + 1 skipped; W100 regression 5/5 pass | issue #56; preserve `Module.runMain()` as the Node preload entry hook |
| W102 | Node module introspection / lookup leaves | 5 | 4/5 files pass; 1 fail; no build | retain four green leaves; park `require.extensions` custom-loader integration |
| W103 | Bun `node:module` / SourceMap leaves | 5 | 3/5 files green; 44 passed / 10 failed / 54 ran / 142 expects; no build | retain three green leaves; park split CJS-loader and malformed-sourcemap diagnostic owners |
| W104 | Bun Node process / stdio leaves | 5 | 5/5 files green; 39 passed / 0 failed / 39 ran / 93 expects; no build | retain all five green leaves; no source owner |
| W105 | Node process identity / timing leaves | 5 | 5/5 files pass; no build | retain all five green leaves; no source owner |
| W106 | Node process exitCode validation leaves | 5 | pre-fix 4/5 pass; post-fix 5/5 pass; issue #57; fresh serialized build | retain all five leaves; keep exitCode owner closed unless a new reproduction reopens it |
| W107 | Node net low-coupling leaves | 5 | 5/5 files pass; no build | retain all five green net leaves; no source owner |
| W108 | Bun Node-net constructor/server leaves | 5 | pre 2/5 green, 147 passed / 20 failed / 175 ran / 300 expects; post 2/5 green, 152 passed / 15 failed / 175 ran / 301 expects | issue #58; retain two green leaves, park node-net multi-owner failures and matcher-only gaps |
| W109 | Node TLS constructor/default-option leaves | 5 | 3/5 pass; 1 fail; 1 timeout; isolated timeout reproduced at one job / 60s | park TLS pauseOnConnect propagation and silent socket-default timeout as separate owners |
| W110 | Bun util low-coupling leaves | 5 | 5/5 green; 57 passed / 0 failed / 57 ran / 966 expects; no build | retain all five green leaves; no source owner |
| W111 | Node child_process basic contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; keep fork/IPC and timeout/kill owners separate |

## W96 delivered slice

W96 started with a fresh five-file Node HTTP/2 probe. Four files passed; the
only failure was `test-http2-client-rststream-before-connect.js`, where the
client readable `end` event and server-side `ERR_HTTP2_STREAM_ERROR` were both
missing from the RST lifecycle. A tiny standalone reproduction confirmed that
the client `_destroy()` hook itself was called once, so the owner was narrowed
to RST event ordering rather than stream destruction dispatch.

Issue [#54](https://github.com/Sunrisepeak/mbun/issues/54) landed in commit
`fa2373e`:

- `modules/jsc/src/js_http2.cppm` now ends the readable side before delivering
  the client reset error, with destruction deferred until the readable
  next-tick completion.
- `modules/jsc/src/js_http2_part2.cppm` routes non-zero server-side peer resets
  through `_destroy`, preserving `ERR_HTTP2_STREAM_ERROR`; CANCEL remains a
  non-error destroy.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W96 Node selector — **5/5 files pass** after the fix; the four pre-existing
  green files stayed green.
- Adjacent Bun HTTP/2/Worker selector — **5/5 files green**, **8 passed / 0
  failed / 8 ran / 8 expects**.
- The standalone lifecycle smoke observed client `end`, client/server stream
  errors, close callback, and `_destroy()` exactly once. It is diagnostic only
  and is not counted as corpus coverage.

No upstream fixture changed and no full corpus/workspace-wide test was run.

## W97 delivered slice

W97 selected five adjacent Node HTTP/2 teardown paths: stream destroy before
connect, session destroy, session close before stream close, shutdown before
connect, and upload rejection. The fresh pre-fix result was **4/5 files pass**;
`test-http2-client-destroy.js` failed because a connect-level AbortSignal made
the pending request receive `ABORT_ERR` instead of Node's
`ERR_HTTP2_STREAM_CANCEL`.

Issue [#55](https://github.com/Sunrisepeak/mbun/issues/55) landed in commit
`ed9c854`:

- the client session consumes the connect AbortSignal exactly once and removes
  it from the underlying net/tls option copy;
- an aborted session still emits `AbortError` / `ABORT_ERR`, while all streams
  in that session are torn down with the pending-stream cancellation error;
- ordinary session teardown keeps its previous open-stream behavior.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W97 Node selector — **5/5 files pass** after the fix.
- W96 Node HTTP/2 regression selector — **5/5 files pass**.
- W95 Bun HTTP/2/Worker regression selector — **5/5 files green**, **8
  passed / 0 failed / 8 ran / 8 expects**.
- Plain and secure connect-abort smoke both observed session `ABORT_ERR` and
  request `ERR_HTTP2_STREAM_CANCEL`; the bounded corpus selector is the
  authoritative result.

No upstream fixture changed and no full corpus/workspace-wide test was run.

## W98 Bun leaf coverage

W98 used five bounded jobs and the W97 coordinator binary without a build. The
selected files were Bun sleep, Deno URLSearchParams, Node X509, TextDecoder,
and Bun require resolution. The result was **4/5 files green**, **161 passed**,
**1 ahead-of-reference**, **165 ran**, and **100,536 expects**.

The four green files were `sleep.test.ts` (**2/2**),
`urlsearchparams.test.ts` (**32/32**), `x509.test.ts` (**14/14**), and
`text-decoder.test.js` (**104/104**). `resolve/require.test.ts` reached **9
passed / 1 ahead-of-reference / 3 todo / 13 ran**: its only failure is a Bun
`test.failing` case that now passes in mbun, so it is recorded as more correct
than the reference rather than treated as an implementation regression.

No source owner was opened from W98, no upstream fixture changed, and no full
corpus/workspace-wide test was run.

## W99 Bun cross-subsystem leaf coverage

W99 used five bounded jobs and the same binary without a build. The initial
five-file result was **4/5 files green**, **108 passed**, **1 failed**, **109
ran**, and **355 expects**. The stable green leaves were:

- `spawn/null-byte-injection.test.ts`: **20/20**;
- `fs/fs-leak.test.js`: **4/4**;
- `streams/pipeTo-signal-leak.test.ts`: **2/2**;
- `web/url/url.test.ts`: **14/14**.

`node-dns.test.js` reached **68/69** in the 5-job probe. Its only failure was
the expected IPv6 address for a public DNS name differing from the answer
returned by the resolver. An isolated 1-job rerun reached **69/69**; a second
five-job rerun reproduced the external answer variance. This is not a stable
runtime owner, so DNS stays parked without a source change.

No upstream fixture changed, no issue was opened, and no full
corpus/workspace-wide test was run.

## W100 Node FileHandle leaf coverage

W100 used five bounded jobs and the existing coordinator binary without a
build. The selected `fs/promises` FileHandle leaves were close-errors,
aggregate-errors, pull, readFile, and writer. All **5/5 files passed**; no
upstream fixture changed and no source owner was opened. This extends W93's
green chmod/stat/truncate/write/sync set without rerunning the full fs subtree.

## W101 Node Module.runMain preload hook

W101 selected five module-loader leaves with five bounded jobs: builtin
identity, createRequire, readonly/wrapper behavior, and the CLI
`runMain` monkey-patch fixture. The pre-fix result was **3/5 files pass**,
**1 skipped** (Windows-only), and **1 failed**. The failure was isolated to
the native entry dispatch: `--require` loaded the preload, but the main file
was evaluated directly, so a preload replacing `Module.runMain` never ran.

Issue [#56](https://github.com/Sunrisepeak/mbun/issues/56) landed in commit
`f296040`. For a Node-dialect entry with preloads, the runtime now calls
`Module.runMain()` after preload execution; Bun continues through its existing
entry wrapper. This restores the observable Node hook without changing the
upstream fixture.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W101 rerun — **4/5 files pass**, **1 skipped**, **0 failed**; the skipped
  fixture is Windows-only.
- W100 FileHandle regression selector — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
the temporary selectors/output were cleaned after verification.

## W102 Node module introspection and lookup leaves

W102 used five bounded jobs and the W101 coordinator binary without a build.
The selected leaves covered `process.config` module-version metadata,
`Module._stat`, builtin-module listing, relative lookup priority, and multi-part
extension resolution. The result was **4/5 files pass** and **1 failed**.

The four green leaves are retained. The only failure is the multi-extension
fixture, which installs mutable `require.extensions` handlers. The current
`node:module` implementation documents custom `require.extensions` loader
integration as deferred native CJS-loader work, so this remains parked without
an issue or speculative source change.

No upstream fixture changed, no build or full corpus/workspace-wide test was
run, and the temporary selector/output were cleaned after verification.

## W103 Bun node:module and SourceMap leaves

W103 used five bounded jobs and the W101 coordinator binary without a build.
The selected files covered the public `node:module` surface, `options.paths`
resolution, SourceMap construction, malformed-map handling, and the concurrent
GC children guard. The aggregate result was **3/5 files green**, **44 passed**,
**10 failed**, **54 ran**, and **142 expects**.

The stable green files were:

- `module-resolve-filename-paths.test.js`: **6/6**, 10 expects;
- `module-sourcemap.test.js`: **3/3**, 6 expects;
- `module-children-concurrent-gc.test.ts`: **1/1**, 2 expects.

The two non-green files have separate owners. `node-module-module.test.js`
reported **21 passed / 9 failed / 30 ran / 102 expects**, spanning builtin
inventory, overridden `_resolveFilename`/`Module.prototype.require`, builtin
cache export shape, `Module.runMain`, and children-tree semantics. The
standalone `sourcemap.test.js` reported **13 passed / 1 failed / 14 ran / 22
expects**: the malformed inline map did not emit the expected decode warning
while preserving the unmapped stack. Current native CJS hook and source-map
stack/diagnostic integration are separate deferred surfaces, so no speculative
issue or mixed fix was opened from W103.

No upstream fixture changed, no build or full corpus/workspace-wide test was
run, and the temporary selector/output were cleaned after verification.

## W104 Bun Node process and stdio leaves

W104 used five bounded jobs and the W101 coordinator binary without a build.
The selected Linux-focused leaves covered synthetic `memoryPressure`, signal
listener install/remove/reinstall, callable process construction, accessor
guards for `setgroups`/`hrtime`, and invalid UTF-16 writes to stdout/stderr.
All **5/5 files passed**, with **39/39 tests**, **0 failures**, and **93
expects**.

Per-file results:

- `process-memory-pressure.test.ts`: **5/5**, 10 expects;
- `process-signal-listener-count.test.ts`: **3/3**, 10 expects;
- `call-constructor.test.js`: **2/2**, 1 expect;
- `process-array-accessor-crash.test.ts`: **5/5**, 6 expects;
- `process-stdio-invalid-utf16.test.ts`: **24/24**, 66 expects.

No source owner was opened, no upstream fixture changed, no build or full
corpus/workspace-wide test was run, and the temporary selector/output were
cleaned after verification.

## W105 Node process identity and timing leaves

W105 used five bounded jobs and the W101 coordinator binary without a build.
The selected leaves covered `process.argv[0]`, `process.uptime()`, symlinked
`process.execPath`, parent-process identity, and Linux `fs.constants.O_NOATIME`.
All **5/5 files passed** with no source owner or upstream fixture change.

No build or full corpus/workspace-wide test was run, and the temporary
selector/output were cleaned after verification.

## W106 Node process exitCode validation leaves

W106 selected five Linux-focused process leaves: high-resolution time,
'process.exitCode' validation and deletion, environment-key deletion, and
timer lifetime tracking. The pre-fix bounded probe was **4/5 files pass**;
the only visible failure was the strict deletion assertion for
'process.exitCode'.

Issue [#57](https://github.com/Sunrisepeak/mbun/issues/57) isolated the owner.
The process slot was wrapped with Node's validation setter but remained
configurable, and JSC's generic non-configurable deletion diagnostic omitted
the property and receiver. The fix in commit 92dd963 makes the slot
non-configurable, formats only the Node process deletion through a narrow
proxy, and preserves invalid 'process.exit(code)' as status 1 instead of
falling through to status 0.

Evidence from the fresh coordinator build:

- bash tools/integration/build_or_die.sh — pass.
- W106 rerun with three bounded jobs — **5/5 files pass**.
- W105 adjacent Node process identity/timing guard — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## W107 Node net low-coupling leaves

W107 reused the fresh W106 coordinator binary and ran five bounded jobs without
a build. The selected leaves covered IPv4 classification, argument
normalization, Socket construction, listening state, and local address/port
reporting. All **5/5 files passed**; no source owner or issue was opened.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
the temporary selectors/output were cleaned after verification.

## W108 Bun Node-net constructor and server leaves

W108 used three bounded jobs and the W106 binary. The fresh pre-fix selector
measured **2/5 files green**, **147 passed**, **20 failed**, **175 ran**, and
**300 expects**:

- 'blocklist-gc.test.ts' and 'node-net-server.test.ts' were green;
- 'socketaddress.spec.ts' had five failures from the Bun matcher
  toThrowWithCode being unavailable in the mbun test harness;
- 'node-net.test.ts' had nine failures across unref/liveness, flowing state,
  AbortError, heap statistics, fd adoption, and reset behavior;
- 'server.spec.ts' had six failures concentrated in constructor/prototype
  shape and default fields.

Issue [#58](https://github.com/Sunrisepeak/mbun/issues/58) isolated the
server-shape owner. Commit 1d755dc aligns the callable constructor parent,
publishes _connections, _unref, _usingWorkers, and highWaterMark, keeps
connection counts synchronized, and matches the Bun EventEmitter prototype
descriptor shape.

Post-fix evidence from a fresh serialized build:

- W108 full selector — **2/5 files green**, **152 passed**, **15 failed**,
  **175 ran**, **301 expects**; five server runtime shape failures were
  removed.
- 'server.spec.ts' focused result — **37 passed / 1 failed / 41 ran / 58
  expects**. The remaining failure is the mbun harness toMatchObject
  own-property treatment of inherited EventEmitter methods; changing the
  real constructor identity to satisfy it would diverge from Bun/Node and is
  parked.
- W107 Node net regression selector — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## W109 Node TLS constructor and default-option leaves

W109 reused the W108 fresh binary and ran five bounded jobs without a build.
The selector measured **3/5 files pass**, **1 fail**, and **1 timeout**:

- TLS server identity checking, no-host connect, and boolean option validation
  passed;
- 'test-tls-server-parent-constructor-options.js' failed only when an accepted
  TLS socket expected pauseOnConnect to remain true;
- 'test-tls-socket-default-options.js' produced no test output and timed out.
  A one-job, 60-second isolated rerun reproduced the same silent timeout, so
  it is not counted as a parallel-load flake.

The two non-green results have different owners and no source change was made:
the pauseOnConnect path needs TLS accepted-socket propagation analysis, while
the silent timeout needs a bounded fixture/liveness investigation. No
speculative issue was opened.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## Delivered slice

The frozen binary reported `test-runner-string-to-regexp.js` as `fail`; the
coordinator binary built with GCC16 reports `pass`. The fix wraps the native
RegExp constructor only to restore Node's invalid-flags diagnostic and repairs
the observable prototype constructor identity. It does not alter successful
RegExp matching or construction.

Evidence:

- `bash tools/integration/build_or_die.sh` — coordinator build passed.
- `python3 tools/integration/node_corpus_runner.py --bin <coordinator-bin> --files <file-list> ...`
  — target file **1/1 pass**.
- Direct smoke — function call, `new`, `instanceof`, constructor identity, and
  invalid-flags error text all passed.
- Regression comparison — `test-runner-option-validation.js` frozen/new both
  **1/1**; `test-util-inspect-regexp.js` and `test-util-inspect.js` frozen/new
  both **0/2**, so those pre-existing inspect failures were not counted as
  regressions.

The next coordinator build also repaired one isolated `util.promisify` contract:

- `internal/util`'s private `customPromisifyArgs` identity is normalized at the
  loader boundary; the public bootstrap implementation now maps multiple
  callback values to the named object.
- Direct smoke with a function decorated by the internal symbol produced
  `{"first":5,"second":17}`.
- `test-fs-readv-promisify.js` is **1/1 pass** after the build.
- `test-util-promisify.js` remains **0/1 file green** because two independent
  warning-contract checks still fail; its custom-args assertion no longer fails.
- The fresh build kept `test-runner-option-validation.js` and
  `test-runner-string-to-regexp.js` at **1/1** each.

The following runner slice wires the existing vendored `SnapshotManager` into
the standalone TestContext:

- `t.assert.snapshot()` and `t.assert.fileSnapshot()` now use one per-process
  manager, including update/read mode and exit-time writes.
- `node:test.snapshot.setResolveSnapshotPath()` and
  `setDefaultSnapshotSerializers()` are exposed.
- `test-runner-snapshot-file-tests.js` is **1/1 pass**; its validation and
  update/read flows pass.
- The multi-file `--test --test-isolation=none` path now works from an arbitrary
  temporary cwd: an update/read round reaches **6/6 pass** for both fixture
  files. The fix discovers the vendored Node `lib` root from the entry file for
  `internal/*` requests and temporarily anchors the snapshot loader during its
  one-time builtin-map initialization.
- Serial `test-runner-snapshot-tests.js` now reaches **33/33 subtests pass**.
  The TAP reporter includes the failed assertion's structured error message, so
  the child negative case observes `Missing snapshots` as Node does. A five-lane
  concurrent probe also hit a shared child-shim `ENOENT`; the serial rerun is
  the authoritative measurement and the shim race is not counted as a runtime
  regression.
- `test-runner-assert.js` now passes the `t.assert` method enumeration and fails
  only on source-expression stack enrichment. No no-op snapshot methods were
  added.

The remaining `util.promisify` warning contract is now a separate green slice:

- The public bootstrap wrapper now retains the original callback-style
  function's return value and emits Node's `DEP0174` deprecation warning when
  that value is a Promise. The existing custom-promisify path is unchanged.
- The original corpus file `test-util-promisify.js` is **1/1 file pass**; both
  warning expectations fire, and the earlier custom-argument assertions remain
  covered by the separate readv test.
- The regression `test-fs-readv-promisify.js` remains **1/1 pass**.
- A three-lane bounded regression probe kept
  `test-runner-snapshot-file-tests.js` at **7/7 assertions** and
  `test-runner-option-validation.js` at **12/12 assertions** while the
  promisify wrapper changed. No full corpus run was started.

A follow-up low-risk `node:util` probe used five parallel bounded lanes. Five
files exited successfully: `test-util-getcallsites.js`,
`test-util-getcallsites-preparestacktrace.js`, `test-util-stripvtcontrolcharacters.js`,
`test-util-types-exists.js`, and `test-util-parse-env.js`. The nearby
`test-util-callbackify.js` lane stayed red on one stack-shape expectation
(`processTicksAndRejections`), while `test-util-format.js`,
`test-util-types.js`, and `test-util-inspect-getters-accessing-this.js` each
hit one distinct semantic blocker. Those three were not changed speculatively.

The next targeted slice added the missing external-value identity for the
internal `JSStream` test handle:

- `util.types.isExternal()` now uses a private WeakSet identity registry, and
  `JSStream` exposes its non-enumerable `_externalStream` test value through
  that registry. Plain objects are not classified as external.
- `test-util-types.js` moved past its first blocker (`undefined` did not match
  `isExternal`) and now reaches the later V8-native-syntax probe for
  `%PrepareFunctionForOptimization`; it remains **1 file fail**, so no new
  green file is claimed from this slice.
- Fresh five-lane regression evidence is all exit 0:
  `test-util-types-exists.js`, `test-util-getcallsites.js`,
  `test-util-parse-env.js`, `test-util-promisify.js`, and
  `test-runner-snapshot-file-tests.js`.

The V8-native-syntax blocker was isolated to optimization-only controls rather
than type semantics:

- Under the explicit `--allow-natives-syntax` flag only, the runtime accepts
  `%PrepareFunctionForOptimization(...)` and `%OptimizeFunctionOnNextCall(...)`
  as no-ops. JSC has no equivalent V8 optimizer controls; ordinary processes
  retain the native `eval` path.
- `test-util-types.js` now exits 0 and exercises the complete type-predicate
  matrix, including the JSStream external value.
- Three parallel fast-call regressions also exit 0:
  `test-buffer-swap-fast.js`, `test-timers-fast-calls.js`, and
  `test-os-fast.js`.

The next measured slice closed the `process.hrtime` argument contract:

- `3427591` validates a supplied previous-time value as an Array of exactly two
  entries, preserving the existing nanosecond subtraction and reporting the
  corresponding Node error codes.
- `test-process-hrtime.js` moved from the observed missing TypeError to exit 0
  after the RED-to-GREEN change. Fresh bounded regressions also exit 0 for
  `test-process-hrtime-bigint.js`, `test-util-types.js`,
  `test-buffer-swap-fast.js`, and `test-runner-snapshot-file-tests.js`.
- This is a targeted runtime result, not a new full-corpus score. The remaining
  `test-whatwg-url-canparse.js` TypeError mismatch stays an independent blocker.

The URL.canParse candidate then closed with a narrow bootstrap fix:

- The failure was in the public `G.URL.canParse` wrapper: its catch-all conversion
  to `false` swallowed the required zero-argument `ERR_MISSING_ARGS` TypeError.
  The vendored internal URL implementation was not the live public path for this
  module shape.
- `800d2a9` adds only the argument-count guard before the existing parse wrapper.
  A fresh coordinator build made `test-whatwg-url-canparse.js` exit 0; a direct
  smoke retained true results for absolute and base-relative URLs.
- Four bounded regressions also exit 0: `test-process-hrtime.js`,
  `test-process-hrtime-bigint.js`, `test-util-types.js`, and
  `test-runner-snapshot-file-tests.js`. No full corpus was rerun.

The assertion source-position probe was also closed as parked for this
checkpoint. `internalBinding('errors').getErrorSourcePositions()` is not the
live path for `t.assert.ok`: the public test assertion uses bootstrap-owned
`AErr`. A narrow internal-binding bridge was built and measured twice, but the
target stayed at **1/2**; the bridge was removed and no unverified behavior was
committed. A future attempt must own the bootstrap assertion/source extraction
boundary as one slice.

The measured `test-util-callbackify.js` candidate was evaluated and parked
without a source commit:

- The existing RED was reproduced. A temporary helper probe confirmed that
  Node's falsy-rejection stack contract needs a `process.processTicksAndRejections`
  frame, while the C++-owned tick queue exposes host `run`/`runTicks` frames.
- A narrow temporary stack-frame compatibility experiment moved past that
  assertion, but the same file then stopped at an independent uncaught-callback
  fixture count (**9** observed error lines versus **7** expected). That generic
  nextTick/uncaught stack boundary is broader than callbackify and was not mixed
  into this lane.
- The experiment was reverted; the working tree has no callbackify change and no
  green file is claimed from this evaluation.

The measured `test-util-format.js` candidate was also parked after root-cause
analysis:

- The RED is the null-prototype class label: mbun reports
  `[Object: null prototype] {}`, while Node reports `[Foo: null prototype] {}`.
- Node's vendored inspect path delegates this case to the V8 internal
  `getConstructorName`, which can recover the original instance constructor after
  `Object.setPrototypeOf(value, null)`. The current JS-only `inspectCtorName` has
  no equivalent information and correctly sees only the null prototype.
- Recovering that name would require global `Object.setPrototypeOf` tracking or an
  engine-level seam, so no speculative global wrapper was added and no green file
  is claimed from this evaluation.

The measured getter-display candidate is now a bounded green slice:

- `a93a26d` makes the public Node inspector collect up to three user-defined
  prototype accessor layers under `showHidden`, invoke getters only when the
  requested `getters` mode allows it, preserve the receiver as `this`, and
  render returned objects with Node's getter label. It also restores the root
  circular-reference marker and applies the default `breakLength` boundary to
  the Node object layout.
- A fresh coordinator build moved
  `test-util-inspect-getters-accessing-this.js` from the measured RED to exit
  0. The first getter assertion, receiver-sensitive value, root `<ref *1>`
  marker, and multiline layout all match the corpus contract.
- Four bounded regressions ran concurrently and all exited 0:
  `test-util-types.js`, `test-runner-snapshot-file-tests.js`,
  `test-process-hrtime.js`, and the getter target itself. No full corpus run
  was started; callbackify, util.format, assertion source-position, and test-v8
  remain separately parked.

The next Node VM probe produced one additional narrow green slice:

- A five-file bounded probe measured `test-vm-create-context-arg.js`,
  `test-vm-is-context.js`, and `test-vm-options-validation.js` at exit 0;
  `test-vm-basic.js` and `test-vm-context.js` were the only two RED files in
  that probe. The latter stopped at one display-error position assertion, not
  at context isolation or argument validation.
- `f3a7393` carries `vm.Script`'s validated `lineOffset`/`columnOffset` into
  Node's decorated error header and first stack frame. The source excerpt and
  caret remain relative to the original source, matching Node's display-errors
  contract.
- A fresh build moved `test-vm-context.js` from exit 1 to exit 0. Five bounded
  lanes then all exited 0: `test-vm-context.js`,
  `test-vm-create-context-arg.js`, `test-vm-is-context.js`,
  `test-vm-options-validation.js`, and the already-green getter target.
  `test-vm-basic.js` remains RED only on JSC's generic `Parser error` versus
  Node's token-specific `Unexpected token '}'` message; that parser-boundary
  mismatch remains isolated rather than receiving a test-specific rewrite.

The adjacent VM property/context probe then separated a larger green surface
from engine-boundary failures without another build:

- Five files exited 0: `test-vm-global-get-own.js`, `test-vm-ownkeys.js`,
  `test-vm-ownpropertynames.js`, `test-vm-ownpropertysymbols.js`, and
  `test-vm-getters.js`. A second five-file probe added green
  `test-vm-cross-context.js`, `test-vm-create-and-run-in-context.js`,
  `test-vm-run-in-new-context.js`, `test-vm-new-script-new-context.js`, and
  `test-vm-new-script-this-context.js`.
- The remaining measured failures were kept separate: global setter error
  wording, global-property enumeration/prototype ownership, and the
  self-referential accessor identity in `test-vm-property-not-on-sandbox.js`.
  These require JSC global-proxy/interceptor semantics or engine-specific error
  information; no broad context mirror rewrite was attempted.
- Across the VM probes in this checkpoint, 15 named files exited 0 and six
  distinct RED boundaries were recorded. This is probe data, not a replacement
  for the repository's full-corpus score; no full corpus run was started.

The first bounded Bun-native probe also hardened the measurement path:

- The initial `bun_corpus_runner.py` invocation exposed a harness-only failure:
  resolving a symlinked `compat/bun` checkout made its discovered files fall
  outside the lexical `--root`. `aafba81` keeps the discovery path lexical and
  adds a symlink regression to the runner self-test; the regression is RED
  before the change and the full self-test is GREEN after it.
- The repaired runner measured five real `compat/bun/test/js/node` files with
  four bounded jobs. The repeated result is **2 green files**, **3 test-failure
  files**, **198/206 passed tests**, and **8 failed tests**. The green files were
  `test/js/node/net/blocklist-gc.test.ts` and
  `test/js/node/tls/node-tls-upgrade.test.ts`; the crypto file reached
  **196/202** with six failures, readline had one timing/stream expectation
  failure, and trace-events had one proxy-network error expectation failure.
- No build was needed for the runner fix and no full Bun corpus run was
  started. The Bun result remains a measured five-file slice, not a new full
  corpus score.

A second Bun-native wave used four parallel one-job runners over adjacent
`test/js/node` subtrees:

- `events/event-emitter.test.ts` is green at **67/67 tests**.
- `console/console-table-iterators.test.ts` is a one-test snapshot formatting
  failure; `url/url-parse-format.test.js` is a one-test invalid-port diagnostic
  failure.
- `process/process-stdin.test.ts` reached **11/14 tests** with three stdin
  behavior failures; one adjacent stale-HUP stdin file timed out at the bounded
  limit. These are kept as process/stream ownership boundaries, not counted as
  green.
- `assert/deep-equal.test.ts` was classified **ahead-of-reference** at
  **229/251 tests** with 22 Bun `test.failing` cases passing in mbun; this is
  not a runtime green file and is retained as a separate classifier result.

A low-cost Bun micro-wave then measured four small subtrees in parallel:

- `os/os.test.js` is green at **52/52 tests** and
  `stream/node-stream-uint8array.test.ts` is green at **5/5 tests**.
- `async_hooks/AsyncLocalStorage.test.ts` reached **32/45 tests** with 11
  failures across async-context propagation, HTTP/HTTP2 cleanup, and plugin
  loading. `string_decoder/string-decoder.test.js` reached **93/95 tests**;
  its two failures are large-buffer allocation/range and output-shape edges.
- This wave used no build and produced only small temporary logs; no cleanup was
  necessary despite the disk gate. The broad async-context and large-buffer
  boundaries remain parked behind their own owners.

The next standard-module Bun wave stayed build-free and produced a high-yield
green sample:

- Four one-file subtrees all exited green: `path/dirname.test.js` (3/3),
  `zlib/deflate-streaming.test.ts` (1/1),
  `dns/dns-lookup-keepalive.test.ts` (1/1), and
  `diagnostics_channel/diagnostics_channel.test.ts` (6/6; 9 tests ran).
- Expanded four-file samples gave `path` **3/4 files green, 9/10 tests** and
  `zlib` **2/4 files green, 9/39 tests**. The path failure is the
  `toNamespacedPath` Windows trailing-slash shape; zlib failures are native
  handle `write`/`writeSync` and lifecycle API ownership gaps.
- No new build was started while swap headroom remained critically low. These
  are measured native slices, not full-corpus score changes.

One more no-build Bun standard-module wave kept the same fast lane:

- `timers.promises/timers.promises.test.ts` is green at **4/4 tests**,
  `perf_hooks/perf_hooks.test.ts` at **8/8**, and
  `promise/reject-tostring.test.ts` at **1/1**.
- `timers/node-timers.test.ts` reached **18/20 tests**; its two failures are
  UTF-16 timer-label formatting and immediate-exception/microtask ordering.
  No runtime-wide timer change was attempted from those two boundaries.

The following four-lane Bun native probe kept only two narrow green slices:

- `dgram/node-dgram.test.js` is green at **3/3 tests** and
  `module/module-children-concurrent-gc.test.ts` at **1/1**.
- `v8/v8-date-parser.test.js` reached **2/5 tests**; the failures are JSC date
  parser semantics, not a broad v8 API absence.
- `test_runner/node-test.test.ts` reached **1/18 tests**; failures span the
  already measured `t.assert` surface, test hooks/async scheduling, nested
  test errors, and mock APIs. It remains parked as a multi-owner test-runner
  boundary rather than a speculative bundle of fixes.

The next single-owner path lane was repaired and rechecked:

- `0ddb3f3` preserves Node's trailing separator for a bare Windows namespace
  root such as `\\\\?\\foo` in `path.win32.toNamespacedPath`. The narrow fix
  is limited to namespace-root shapes and leaves UNC, device, drive-root, and
  ordinary path handling on the existing branches.
- A fresh Linux build completed successfully. The target Bun file is now
  green at **4/4 tests**; three unrelated regression lanes stayed green:
  `events/event-emitter` **67/67**, `os/os` **52/52**, and
  `timers.promises` **4/4**. The four-file run therefore measured
  **127/127 tests**, **0 failed**, with **542 expects** across the selected
  files.
- The resource gate remained open for this single build: about **43 GiB
  available memory**, only **41 MiB swap headroom**, and about **24 GiB free
  disk**. The stale-cache dry-run found approximately **0 MiB** safely
  reclaimable, so no cleanup was performed and no full corpus run was started.
- Reusing the fresh binary, the previously expanded four-file path sample
  moved from **3/4 files and 9/10 tests** to **4/4 files and 10/10 tests**:
  `dirname` **3/3**, `is-absolute` **2/2**, `to-namespaced-path` **4/4**, and
  `win32-exists` **1/1**. This was a no-build confirmation of the adjacent
  path slice, not a full path-corpus claim.

The next measured console owner also closed with one narrow runtime change:

- `5f36ae9` aligns `Console#table` with the vendored Bun `ConsoleObject` model:
  cells are centered within their display-width column and an odd spare space
  is placed on the right. The prior implementation incorrectly used Node's
  left-aligned CLI-table rule.
- The target `console/console-table-iterators.test.ts` moved from **0/1** to
  **1/1**. Fresh-build regression lanes stayed green for
  `path/to-namespaced-path` **4/4**, `events/event-emitter` **67/67**, and
  `os/os` **52/52**; the four-file run measured **124/124 tests**, **0 failed**,
  and **537 expects**.

A four-file triage probe then separated the next candidates without another
build:

- `url/url-parse-format.test.js` reached **4/6 tests** with one failure and
  one TODO. Its invalid-port expectation conflicts with the already-green
  Node invalid-input contract, so the existing cross-corpus conflict remains
  parked rather than routing on test identity.
- `process/process-stdin.test.ts` reached **11/14 tests**; the three failures
  are distinct stdin stream behaviors (file helper output, paused data
  delivery, and explicit read pull semantics), not one formatting owner.
- `v8/v8-date-parser.test.js` reached **2/5 tests**; all three failures are
  JSC date-parser semantic differences. The four-file probe measured
  **17/26 tests passed**, **8 failed**, with the console target already closed
  in the preceding fresh-build wave.

The cross-corpus guard then caught and isolated two dialect conflicts:

- The initial Node guard exposed the expected opposite contracts: Node's table
  cells are left-aligned and Node's bare namespace root has no trailing slash.
  `cea1bc4` routes these two choices through the existing process-level
  `__mbunDialect`; it does not inspect test paths or weaken either corpus.
- After a fresh build, Bun's four-file guard remained **124/124 tests green**.
  Node `test-path-makelong.js` and `test-path-resolve.js` both passed, and a
  bounded Node custom smoke confirmed left-aligned table rows plus the
  no-trailing-slash namespace result.
- The broader Node `test-console-table.js` guard still stops at its separate
  Map-iterator Key/Values shape gap; `test-console.js` still stops at the
  separate `_times` private-field gap. Neither is counted as a new regression
  from the dialect patch.

A bounded Node test-runner probe was used to choose the next strategy:

- `test-runner-get-test-context.js` passed **1/1**.
- `test-runner-cli.js` failed at fixture discovery/cwd resolution;
  `test-runner-diagnostics-channel.js` failed on bindStore/event payload
  propagation; and `test-runner-error-reporter.js` failed on reporter failure
  counts. These are three different ownership boundaries, not one cheap
  formatting fix.
- The probe therefore measured **1/4 files green** and keeps the remaining
  test-runner work in the port/graft track. No speculative patch or full
  test-runner sweep was started.

A build-free Bun.Terminal coverage checkpoint then confirmed the newer PTY
surface on Linux:

- Four files were green under four bounded jobs: `terminal.test.ts` **94/94**,
  `terminal-spawn.test.ts` **16/17** with one declared skip,
  `terminal-platform-gaps.test.ts` **19/19**, and
  `spawn/spawn-path.test.ts` **1/1**.
- The aggregate was **130 passed, 1 declared skip, 0 failed, 336 expects**.
  This is a real runtime result for the selected files, not a ported-symbol
  claim; no build or full Bun corpus run was started.

### W41 stream-like spawn stdin：async pipe owner 实测推进

- Wave 46 的四文件基线为 `spawn-stdin-readable-stream` **7/30**、
  `spawn-streaming-stdout` **1/1**、`spawnSync` **6/16**，另有 broad
  `spawn.test` 在 30 秒边界超时；四文件合计 **32 passed、10 failed**。
- `90d895d` 将真实 `ReadableStream` 与 async iterable stdin 接入已有
  `spawnEx`/`__mbun_io_tick` 路径；reader/iterator 在 child exit 时分别走
  `cancel()`/`return()`，普通 EOF 仍通过 `stdin.end()` 收口。为绕开
  `process_web.cppm` 已接近 GCC constexpr 字符串上限，适配器保持在相邻
  `bun_spawn_stream.cppm` payload 分区。
- fresh Linux build 成功（约 **61 秒**）。同一四文件 bounded probe（4 jobs、
  30 秒/文件）后为：stream stdin **27/30 passed、1 failed、2 TODO**，其中
  两项 async-iterable child-exit 用例已通过；streaming-stdout 仍为 **1/1**、
  **211 expects**；四文件合计 **34 passed、8 failed、47 ran、288 expects**。
- stream 文件唯一剩余失败是其 upstream `ReadableStream object type count`
  的 50-child burst，在 `spawnEx: fork() failed` 处触发 native spawn-burst
  资源边界；单文件复测仍复现，因此不归因于四 lane 并发，也不继续扩大本
  owner 的改动。随后以单文件 `MemoryMax=34G、TasksMax=1024` bounded scope
  复跑完整 stream 文件，结果为 **28 pass、2 TODO、0 fail**，说明源适配本身
  已闭合，默认 runner 的 **4G/512** 安全 profile 才是该异常 burst 的测量边界。
  `spawnSync` 与 broad `spawn.test` 维持原有独立 owner。
- 构建后资源记录约 **44 GiB available memory**、swap 可用约 **163 MiB**、
  根分区可用约 **23 GiB**；临时构建产物 dry-run 未发现安全可回收量，未做
  清理，未启动全量 corpus。

### W48 Linux 无构建近绿筛选：Bun/Node owner 再分流

- Bun 四文件 bounded probe（4 jobs、30 秒/文件）合计 **35/45 tests passed**、
  **9 failed、355 expects**：`process-stdin` **11/14**（paused data、file
  helper、explicit read 三个 stdin 语义缺口），`node-timers` **18/20**（UTF-16
  timer label 与 exception/microtask ordering），`url-parse-format` **4/6**
  （invalid-port 方言冲突），`v8-date-parser` **2/5**（JSC date parser semantics）。
- Node 四文件 bounded probe 为 **1/4 files pass**：`test-vm-context.js` 已绿；
  `test-util-callbackify.js` 剩余 `processTicksAndRejections` stack shape，
  `test-util-format.js` 剩余 null-prototype object 的 constructor label，
  `test-vm-basic.js` 剩余 JSC `Parser error` 与 Node token-specific message
  差异。三条红测 ownership 不同，本轮不做跨 owner 混修或新构建。
- 策略调整：保留 stream stdin 的 source checkpoint；资源 profile 作为独立
  runner/tooling owner，W48 的 Node/Bun 红测分别停车到 stack/inspect/parser/
  dialect 专项；下一轮只从可由单一运行时边界闭合的近绿项选任务，继续维持
  3–5 个 bounded lanes，暂不跑全量 corpus。

### W49 fresh-binary 回归闸门

- 复用当前最新二进制并行复跑四条既有绿线：Bun
  `events/event-emitter`、`os/os`、`timers.promises`、
  `stream/node-stream-uint8array`。
- 结果为 **4/4 files green、128/128 tests、0 failed、565 expects**；这是
  stream-like stdin source checkpoint 的邻接回归证据，不替代 Bun/Node 全量分数。
- 资源保持在约 **44 GiB available memory / 164 MiB swap / 23 GiB free disk**，
  无新构建、无全量 corpus、无临时产物清理。

### W50 spawn-heavy resource profile tooling

- Issue [#37](https://github.com/Sunrisepeak/mbun/issues/37) 记录并脱敏了
  `spawn-stdin-readable-stream` 的 scope-induced `fork()` 边界；设计记录见
  `20260801-bounded-resource-profile-design.md`。
- `1449975` 为 `bun_corpus_runner.py` 增加显式 `--memory-max` /
  `--tasks-max`，默认仍为 **4G/512**；非默认 profile 强制 `--jobs 1`，summary
  写入实际 profile，未按文件名自动放宽，也未改 upstream corpus。
- runner self-test、journal self-test、Python syntax check 均通过。真实
  `spawn-stdin-readable-stream` 单 lane 使用 `34G/1024` 后为 **1 file green、
  28/30 pass、0 fail、2 TODO、61 expects**；这是此前默认 profile `27/30`
  加资源对照后的可复现闭合结果。
- 本节点无构建、无全量 corpus；高资源 scope 只运行 1 lane，未改变 3–5 lane
  常规并行策略，资源水位约 **44 GiB available / 164 MiB swap / 23 GiB free disk**。

### W51 process.stdin final read/end ordering

- Issue [#38](https://github.com/Sunrisepeak/mbun/issues/38) 记录了一个单一
  `process.stdin` owner：无 `readable` listener 的 `read(3)` 在返回最后的 `gh`
  之前同步触发 `end`，使 `end` listener 只能看到 `['abc', 'def']`，而调用方
  最终实际收到 `['abc', 'def', 'gh']`。
- `bootstrap.cppm` 只将最后一次 `emitEnd()` 放到当前 `read()` 返回后的
  `process.nextTick`，不改变 EOF、readable buffer 或 close 语义。root `mcpp build`
  成功，release 构建耗时约 **60 秒**。
- focused `process-stdin.test.ts` 从既有 **11/14** 提升到 **12/14**，**24 expects**；
  `read(n)` 目标通过，剩余两项明确为不同 owner：`Bun.file()` child stdin 的
  ref 形态，以及 stdout WebStream 的 disturbed/reject 语义。
- W48 四文件 triage 在新 binary 上为 **36 passed、8 failed、45 ran、355 expects**，
  相比 **35/45、9 failed** 净增一条通过；四条既有 green guards 保持
  **4/4 files、128/128 tests、0 failed、565 expects**，使用默认 **4G/512、4 jobs**。
- 资源保护：一次 workspace-wide 构建在 swap 降至约 **43 MiB**、磁盘约 **20 GiB**
  时停止，随后只完成 root 窄构建；无全量 corpus，未删除源码或必要 fresh binary，
  只保留可复核的 bounded 结果。

### W52 Bun.spawn file-backed stdin owner

- Issue [#39](https://github.com/Sunrisepeak/mbun/issues/39) 记录了第二个
  `process-stdin` owner：`Bun.file()` 被错误降级为匿名 pipe，child 的
  `typeof process.stdin.ref` 为 `function`，而 regular-file stdin 的 Node
  对照为 `undefined`；相邻 `stdin: "pipe"` 仍为 `function`。
- 根因修复分两点：regular-file `Bun.file()` 在 `bun_spawn_stream.cppm` 中先以
  fd stdio 传给 child，并在 spawn 边界后关闭 parent fd；bootstrap 只对非
  regular-file fd 0 安装 `ref/unref`。generic Blob、byte、ReadableStream、
  async iterable 和 keyword stdin 路径没有改动。
- 首次 root 构建因 `process_web` raw payload 超过 GCC constexpr 字符串长度上限
  失败；随后按现有 payload 分区规则移到 `bun_spawn_stream.cppm`，root release
  build 成功，耗时约 **60.26 秒**。这是构建边界处理，不是运行时失败。
- focused `process-stdin.test.ts` 从 **12/14** 提升到 **13/14、26 expects**；
  `file does the right thing`、pipe 对照和真实 file-byte smoke 均通过。唯一
  剩余失败是 stdout WebStream 被重复消费后没有 reject，归入独立 WebStream
  disturbed/reject owner。
- W52 四文件 post-fix bounded probe：**126 passed、8 failed、134 ran、3015
  expects**；修复前为 **125/134、9 failed、3013 expects**。四条既有 green guards
  保持 **4/4 files、128/128 tests、0 failed、565 expects**。
- 资源策略保持 root 窄构建、默认 **4G/512** runner 和 bounded lanes；未启动
  workspace-wide 或全量 corpus。构建与测试期间 swap 仍约 **43 MiB free**、磁盘
  约 **20 GiB free**，后续新 owner 需继续避免 broad build。

### W53 Bun.spawn stdout disturbed/reject owner

- Issue [#40](https://github.com/Sunrisepeak/mbun/issues/40) 记录了 W52 留下的
  最后一个 `process-stdin` 红测：`new Response(proc.stdout).text()` 消费 custom
  stdout adapter 后，第二次 `proc.stdout.text()` 错误地再次 resolve。最小 smoke
  同时确认 direct helper 二次消费、async iterator 后 helper、`pipeTo` 后 helper
  都缺少共享的 used-state；native `ReadableStream` 的 `consumerUsableError`
  已有对应语义。
- `04214b1` 在 `bun_spawn_stream.cppm` 的 payload 分区中只包装 live subprocess
  readable adapter，为 `text/bytes/arrayBuffer/blob/json`、async iterator 与
  `pipeTo` 共用一次 claim；generic Blob stdin 与已有 pipe/stream 路径不变，避免
  再次扩大接近 constexpr 上限的 `process_web` payload。
- focused `process-stdin.test.ts` 从 **13/14、26 expects** 提升到 **14/14、27
  expects**，成为本轮第一个 focused green；4-file adapter lane 使用默认
  **4G/512、4 jobs**，结果为 **28/46 passed、18 failed、410 expects**。其中
  `readablestream-helpers` 的 10 个 Bun.spawn conversion checks 全通过，18 个
  wrong-this failures 是该文件既有的 `ReadableStream.prototype.*` contract，
  未混入本 owner；`spawn-streaming-stdout` 与 `spawn-streaming-stdin` 均 green。
- 四条既有 guards 复测仍为 **4/4 files、128/128 tests、0 failed、565 expects**。
  root release build 成功，耗时约 **60.21 秒**；未启动 workspace-wide build，未跑
  全量 corpus。资源策略继续保持 3–5 个 bounded lanes；本轮只在一个 adapter lane
  中使用 4 jobs，并继续回避 swap/disk 低水位下的并发构建。

### W54 ReadableStream conversion-helper brand owner

- Issue [#41](https://github.com/Sunrisepeak/mbun/issues/41) 记录了 adapter lane
  暴露的独立 contract：`ReadableStream.prototype.text/json/bytes` 对非法 receiver
  返回 rejected Promise，`blob` 还在 brand check 前读取 receiver 属性；Node/Bun
  原生 contract 要求同步 `ERR_INVALID_THIS`。
- `a8a6c05` 在 `js_streams.cppm` 为五个 conversion helper 复用已有
  `isReadableStream()` brand predicate，非法 receiver 立即抛出
  `Value of "this" must be of type ReadableStream`，valid stream 的消费与
  locked/used Promise 错误路径不变。
- `readablestream-helpers.test.ts` 从 **12/30、18 failed** 提升到 **30/30、0
  failed、43 expects**。W54 六文件 bounded lane 使用默认 **4G/512、4 jobs**，合计
  **6/6 files green、172/172 tests、0 failed、635 expects**；额外最小 smoke 验证
  五个方法对非法 receiver 都同步给出 `ERR_INVALID_THIS`，valid `text` 与
  `arrayBuffer` 仍正常转换。
- root release build 成功，耗时约 **60.50 秒**；未启动 workspace-wide build，未跑
  全量 corpus。当前资源约 **44 GiB available memory、43 MiB swap free、20 GiB
  free disk**，继续暂停 broad build，仅保留 bounded lane。

### W61 Node buffer.transcode owner

- Fresh Linux bounded probe before implementation used five jobs over four Node
  candidates plus the existing Buffer.fill guard: **1/5 files pass**. The
  target `test-icu-transcode.js` failed immediately because `buffer.transcode`
  was not exposed; the three URL files failed independent formatting/error
  contracts and stayed outside this owner.
- Issue [#44](https://github.com/Sunrisepeak/mbun/issues/44) adds the Node-only
  `buffer.transcode` module export in
  `modules/jsc/src/builtins/node_buffer_extra.cppm`. It accepts Buffer and
  Uint8Array input, covers the corpus's utf8/latin1/ascii/utf16le/ucs2 aliases,
  maps unrepresentable latin1/ascii code points to `?`, and preserves Bun's
  existing undefined exports.
- The focused acceptance file passed **1/1**. The target plus ten Buffer/Node
  guards passed **11/11 files** with three jobs. The original five-file probe
  moved to **2/5 files pass**; the three URL failures remain independent.
  Bun-dialect smoke kept both transcode exports `undefined`. Root release build
  passed in **60.12 seconds**. No full corpus or workspace-wide build was run.

### W62 Node URL custom-inspect owner

- The W61 five-file candidate set had three URL/Buffer owners after transcode:
  URL custom inspect, URL custom parsing, and URL custom setters. The inspect
  failure was isolated to the existing Node URL custom-inspect hook: JSON
  double quotes, visible `toJSON`/`toString`, missing `showHidden` context, and
  a fixed `URL` header.
- Issue [#45](https://github.com/Sunrisepeak/mbun/issues/45) changes only the
  Node-dialect custom inspector in `node_util_extra.cppm`: Node quote/field
  order, hidden URLContext, dynamic subclass name, and depth-zero output. Bun
  output and generic URL parser/setter paths are unchanged.
- Focused `test-whatwg-url-custom-inspect.js`: **1/1 pass**. The complete
  five-file candidate set moved to **3/5 pass**: transcode, URL inspect, and
  Buffer.fill green; custom parsing and custom setters remain parked as
  independent owners. Root release build passed in **60.24 seconds**. No full
  corpus or workspace-wide build was run.

### W63 Node URL custom-parsing triage parked

- The remaining W61/W62 URL candidate was measured as a message mismatch:
  native URL errors exposed input/base details instead of Node's `Invalid URL`.
  A diagnostic-only Node wrapper normalized that message, but the focused file
  then revealed **9 invalid inputs accepted by the native parser**, so the
  candidate is a parser-algorithm cluster rather than a safe message-only owner.
- The experiment was reverted with **no source commit** and no green claim.
  URL custom setters remain a separate lone-surrogate Unicode owner. The
  transcode, URL inspect, and Buffer.fill green slices stay closed.
- The diagnostic root build passed in **60.76 seconds**; the bounded focused
  run remained red. No full corpus or workspace-wide build was performed, and
  no new issue was mixed into the parser cluster.

### W64 Node URL setter USVString owner

- Issue [#47](https://github.com/Sunrisepeak/mbun/issues/47) isolates the
  remaining URL setter contract: Node applies WebIDL `USVString` conversion
  to URL setters, while the native path rejected lone surrogates and accepted
  Symbol/object values with the wrong observable behavior.
- The Node-only wrapper in `modules/jsc/src/builtins/node_util_extra.cppm`
  now applies JavaScript `ToString`, rejects Symbols with the Node TypeError,
  and normalizes lone surrogates through `util.toUSVString` for `href`,
  `protocol`, `username`, `password`, `host`, `hostname`, `port`, `pathname`,
  `search`, and `hash`. Bun behavior and the parked parser path are unchanged.
- Focused `test-whatwg-url-custom-setters.js`: **1/1 pass**. The five-file
  bounded regression used **5 jobs** and measured **4/5 files pass**: setter,
  transcode, URL inspect, and Buffer.fill are green; URL custom parsing remains
  the W63 parked failure with the same native message mismatch.
- The final root release build passed in **60.86 seconds**. No full corpus or
  workspace-wide build was started; resource policy remains serialized builds
  plus 3–5 bounded test lanes.

### W65 Bun URL API leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The selected
  leaves were `url-parse-query`, `url-format`, `url-format-whatwg`,
  `url-domain-ascii-unicode`, and `url-canParse-whatwg`.
- All **5/5 files** were green: **135 passed, 0 failed, 137 ran, 130
  expects**. Two framework-level skips are included in the ran/pass total as
  reported by the Bun runner; no test failure or timeout occurred.
- This is additive Bun coverage data, not a claim about the parked URL parser
  cluster or the complete URL subtree. No full corpus or workspace-wide build
  was started; the next route remains a fresh one-owner Node/Bun near-green
  measurement under the 3–5 lane resource policy.

### W66 Bun util and parse_args leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The selected
  leaves covered MIME APIs, both `parse_args` files, AbortSignal resource
  handling, and the exact MIME fixture.
- Four files were green: **128 passed, 0 failed, 128 ran, 273 expects**.
  `mime-api`, `parse_args/default-args`, `parse_args/parse-args`, and
  `test-aborted` all passed. `util/exact/mime-test.js` exited successfully but
  registered **0 tests** and is classified `no-tests`, not green coverage.
- This checkpoint adds module-level coverage without changing the runtime or
  conflating no-test fixtures with passing tests. No full corpus or
  workspace-wide build was started; continue with a fresh one-owner row and
  3–5 bounded lanes.

### W67 Bun worker_threads triage parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `worker-thread-id`, `worker-top-level-await`, `worker-shutdown-post-leak`,
  `worker-async-dispose`, and `worker-transfer-list`.
- The runner measured **2 green files, 8 passed, 3 failed, 12 ran, 13
  expects**; one additional file was `all-skipped`. `worker-async-dispose`
  and `worker-transfer-list` were fully green. The direct worker-thread-id
  entry is a fixture-style file and fails without its parent worker context,
  so it is not an independent runtime owner.
- `worker-top-level-await` reproduced serially at **4/6 tests pass, 2 fail**.
  Both failures are the same unsettled-TLA exit-code 13 contract. The existing
  engine evidence and prior rollback show that applying Node's status 13 in the
  shared path removes two Bun green files and exposes a separate worker-open
  liveness gap; this remains parked as a cross-corpus/liveness boundary, with
  no speculative source patch or mixed issue.
- No full corpus or workspace-wide build was started. Continue with a fresh
  one-owner row under the 3–5 lane policy.

### W68 Bun process leaf probe and fixture triage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `process-nexttick`, `process-args`, `process-exitCode-with-exit`,
  `process-on`, and `process-stdio-invalid-utf16`.
- The runner measured **3 green files, 30 passed, 6 failed, 36 ran, 131
  expects**. The green files were `process-args` (**1/1, 50 expects**),
  `process-on` (**3/3, 5 expects**), and invalid-UTF-16 stdio
  (**24/24, 66 expects**).
- `process-exitCode-with-exit.js` is a fixture-style entry: direct `bun test`
  invocation leaves a non-numeric final argv and produces NaN, while a direct
  smoke with an explicit numeric argument prints `PASS`; it is not an
  exitCode setter owner. `process-nexttick` reached **2/7 tests pass** with
  callback validation, queue ordering, and repeated scheduling failures, so
  it remains a multi-owner process boundary.
- No full corpus or workspace-wide build was started. Keep the three green
  process leaves as coverage guards and continue from a fresh one-owner row.

### W69 Bun module loader probe parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  module resolve paths, module metadata, custom require extensions, and two
  sourcemap files.
- The runner measured **2 green files, 46 passed, 17 failed, 63 ran, 158
  expects**. `module-resolve-filename-paths` was **6/6** and
  `module-sourcemap` was **3/3** green.
- `node-module-module` reached **21/30** with failures across builtin list
  size, overridden resolve/require hooks, builtin cache/export shape, and
  Module.runMain/children. `require-extensions` reached **3/10** with custom
  loader and extension mutation failures. These are separate loader owners.
- `sourcemap` reached **13/14**; the sole failure is the missing
  `Could not decode sourcemap` warning in an entry runtime error stack. The
  `node:module SourceMap` class itself passed its API/VLQ cases, while the
  runtime warning/stack integration has no narrow existing owner. Park this
  with the loader cluster rather than changing the class or global error path.
- No full corpus or workspace-wide build was started. Keep the two green
  module guards and choose the next fresh one-owner row.

### W70 Bun filesystem leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `fs/dir`, `fs-mkdir`, `fs-promises-writeFile-async-iterator`,
  `fs-stats-constructor`, and `fs-stats-truncate`.
- All **5/5 files** were green: **52 passed, 0 failed, 55 ran, 138 expects**.
  This adds filesystem leaves across directory operations, mkdir validation,
  async-iterator writes, and Stats construction/truncation without touching
  the broader fs error or platform-specific clusters.
- No full corpus or workspace-wide build was started. Keep these five files as
  bounded filesystem guards and continue with a fresh one-owner row under the
  3–5 lane policy.

### W71 Bun HTTP leaf probe parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  numeric headers, response timeout/unref, early hints, HTTPParser, and
  transfer-encoding/trailer handling.
- The runner measured **3 green files, 22 passed, 13 failed, 35 ran, 68
  expects**. `numeric-header`, `node-http-res-settimeout-unref`, and
  `early-hints-crlf-injection` were green.
- `node-http-parser` and `node-http-transfer-encoding` remain parked: their
  failures span parser state transitions, buffer ownership, header/trailer
  validation, connection ordering, and timeout/liveness behavior. No single
  safe HTTP owner was inferred and no full HTTP subtree was scanned.
- No full corpus or workspace-wide build was started. Keep the three green
  HTTP guards and select the next fresh one-owner row.

### W72 Bun child-process leaf probe and IPC owner

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `child_process_send_cb`, child-process stdio, child-process exec,
  `child-process-rlimit-nofile`, and `child_process_ipc`.
- The runner measured **4 green files, 20 passed, 1 failed, 21 ran, 44
  expects**. `child_process_send_cb` (**1/1**), `child-process-stdio`
  (**7/7**), `child-process-exec` (**11/11**), and
  `child-process-rlimit-nofile` (**1/1**) were green.
- `child_process_ipc` is a narrow compatibility failure: its fixture passes
  an unlistened `net.Server` (`_fd === -1`) to `child.send()`. Node returns
  `true`, invokes the callback with `null`, and does not report a handle error;
  mbun emits an extra `uncaughtException ERR_INVALID_HANDLE_TYPE` before the
  expected five output lines. A focused Node probe confirmed the reference
  behavior. Issue [#48](https://github.com/Sunrisepeak/mbun/issues/48) tracks
  the smallest branch: treat this no-descriptor server as a plain message,
  while retaining rejection for genuinely unsupported handles.
- Issue #48 was fixed in `d390ad7`: `child_process.send()` now recognizes an
  actual unlistened `net.Server` and sends the message without a
  `NODE_HANDLE` frame, while fake or unsupported handles still return
  `ERR_INVALID_HANDLE_TYPE`. The root release build completed in **60.59s**.
- The focused guard moved from **1/1 failed** to **1/1 green**. The complete
  W72 five-file regression with **5 bounded jobs** is now **5/5 files green,
  21 passed, 0 failed, 21 ran, 44 expects**. The upstream fixture remains
  read-only; no full corpus or workspace-wide build was started.

### W73 Node crypto leaf coverage and checkPrime snapshot owner

- A fresh five-file Node crypto probe used **5 bounded jobs** and reused the
  current Linux binary before the fix. It measured **4 green files, 33
  passed, 2 failed, 35 ran, 85 expects**; HMAC algorithm validation,
  invalid-this handling, lazy hash, and HKDF callback-null behavior were
  already green.
- Both failures were in `crypto-random.test.ts`: sync and async `checkPrime`
  must snapshot candidate bytes before evaluating the `options.checks` getter,
  and must read that getter exactly once. Issue [#49](https://github.com/Sunrisepeak/mbun/issues/49)
  owns this single `crypto_asym.cppm` argument-order owner.
- `b2b5bae` copies normalized candidate bytes before option evaluation and
  stores `options.checks` in one local read. Root release build completed in
  **60.40s**. The same five-file bounded regression is now **5/5 files green,
  35 passed, 0 failed, 35 ran, 87 expects**.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Keep the five green crypto leaves as guards and choose the
  next fresh one-owner row.

### W74 Node events/stream/assert/console leaf probe parked

- A fresh five-file Node leaf probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **4 green files, 136 passed, 2 failed, 138 ran, 279 expects**.
- `event-emitter` (**67/67**), `node-stream-uint8array` (**5/5**),
  `assert-typedarray-deepequal` (**45/45**), and
  `console-table-iterators` (**1/1**) were green and are retained as guards.
- `node-timers` reached **18/20**. Its two failures are separate owners: a
  JSC UTF-16 string representation assertion in `clearTimeout`, and
  immediate-exception/microtask ordering in a spawned fixture. No mixed fix
  was attempted and no issue was opened for the multi-owner file.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Continue with a fresh one-owner row under the 3–5 lane
  policy.

### W75 Bun util leaf coverage

- A fresh five-file Bun-native util probe used **5 bounded jobs** and reused
  the current Linux binary; no source change or build was needed. All **5/5
  files** were green: **13 passed, 0 failed, 13 ran, 24 expects**.
- The guards cover Bun file existence, `Bun.concat`, error-code mirroring,
  error-name preservation, and file-type detection. Keep them as cheap Bun
  leaves while selecting the next fresh candidate.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W76 Node assert leaf probe and ahead-of-reference matrix

- A fresh five-file Node assert probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. Four files were
  green and the fifth was `ahead-of-reference`: **275 passed, 22 failed, 297
  ran, 451 expects**.
- `assert-doesNotMatch` (**3/3**), `assert-match` (**3/3**), `assert-promise`
  (**12/12**), and `assert.spec` (**28/28**) were green. `deep-equal` reached
  **229/251**; all 22 failures are upstream `test.failing` cases where mbun
  passes a case Bun currently expects to fail, spanning prototypes, own
  properties, RegExp state, and collection/typed-array semantics. The runner
  therefore classified it `ahead-of-reference`, not green.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Keep the four assertion guards and leave the multi-semantic
  deep-equality matrix split for a later measured owner.

### W77 Node crypto leaf probe parked on external-memory owner

- A fresh five-file Node crypto probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **4 green files, 51 passed, 8 failed, 59 ran, 344 expects**.
- `crypto-sign-regression` (**1/1**), `x509` (**14/14**), `scrypt` (**1/1**),
  and `crypto-oneshot` (**35/35**) were green. `crypto-extra-memory` had
  **0/8** green tests: all failures are `heapStats().extraMemorySize` deltas
  for SecretKey, asymmetric keys, Hash/Hmac/Cipher, ECDH, Sign, and Verify.
- The failures share the JSC GC/native-wrapper external-memory accounting
  boundary, not one crypto algorithm. No mixed crypto patch was attempted;
  keep the four green leaves and park this architectural owner.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W78 Bun file/util leaf probe parked on Bun object internals

- A fresh five-file Bun-native util/file probe used **5 bounded jobs** and
  reused the current Linux binary; no source change or build was needed. It
  measured **4 green files, 27 passed, 1 failed, 28 ran, 227 expects**.
- `bun-file-fd-read` (**3/3**), `bun-file-read` (**1/1**),
  `bun-isMainThread` (**1/1**), and `fileUrl` (**20/20**) were green.
- `BunObject` reached **2/3**: its only failure is the missing
  `bun:internal-for-testing.hasNonReifiedStatic` helper used to check Bun's
  lazy static-property reification before/after spreading the Bun object.
  This is a Bun object/bootstrap-internals owner, not a file API regression;
  it is parked without a test-specific shim.
- The probe emitted a local environment dump while printing the Bun object;
  that temporary output was deleted and no environment values, paths, or
  identifiers were copied into project docs, commits, or PR text.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W79 Bun crypto probe parked on WebCrypto validation matrix

- A fresh three-file Bun-native crypto probe used **3 bounded jobs** and
  reused the current Linux binary; no source change or build was needed. It
  measured **2 green files, 9,515 passed, 736 failed, 10,251 ran, 29,406
  expects**.
- `cipheriv-decipheriv` (**13/13**) and `x25519-derive-bits` (**12/12**) were
  green. The WPT `generateKey` file reached **9,490/10,226**; its failures
  repeat across empty-algorithm and RSA algorithm-property validation cases
  with exception-type/validation mismatches. This is a broad WebCrypto
  validation matrix, not a single safe crypto primitive owner, so it is
  parked without a speculative patch.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W80 Node zlib leaf probe parked on native-handle boundary

- A fresh five-file Node zlib probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **3 green files, 17 passed, 30 failed, 47 ran, 64 expects**.
- `bytesWritten` (**5/5**), `deflate-streaming` (**1/1**), and
  `zlib.kMaxLength.global` (**8/8**) were green. The two remaining files
  failed across native handle bounds/writeState, missing handle methods,
  init-after-close validation, and onerror re-entrancy. These are a shared
  zlib binding lifecycle/handle surface rather than one safe leaf owner, so
  no mixed fix was attempted.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W81 Node console/zlib leaf probe parked on mixed zlib failures

- A fresh five-file Node console/zlib probe used **5 bounded jobs** and reused
  the current Linux binary; no source change or build was needed. It measured
  **4 green files, 390 passed, 12 failed, 404 ran, 480 expects**.
- `console-constructor-exception` (**1/1**), `console` (**7/7**), zlib
  `leak` (**8/8**), and `zlib-reset-race` (**3/3**) were green. `zlib.test`
  reached **371/385**; its 12 failures span invalid raw data, libdeflate
  level validation, chunk/output bounds, and async buffer lifetime. No single
  safe owner was inferred, so no mixed zlib patch was attempted.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W82 Node path leaf coverage

- A fresh five-file Node path probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. All **5/5 files**
  were green: **13 passed, 0 failed, 13 ran, 0 `expect()` calls**. These
  assert-style files cover basename, dirname, extname, isAbsolute, and join.
- The zero `expect()` count is a runner metric, not missing assertions: the
  upstream files use Node's assert APIs. No upstream fixture changes, no full
  corpus, and no workspace-wide build were performed.

### W83 Bun path format dialect fix

- The fresh five-file Bun path probe used **5 bounded jobs**. Before the source
  change it measured **4/5 files green, 15 passed, 1 failed, 16 ran, 9
  expects**. The sole failure was `path.format(null)`: the Bun corpus expects
  `The "pathObject" property must be of type object, got object`, while the
  shared validator emitted Node's `The "pathObject" argument must be of type
  object. Received null` form.
- Issue [#50](https://github.com/Sunrisepeak/mbun/issues/50) isolated the
  owner. Commit `7f6af93` makes only this invalid-object message dispatch on
  `globalThis.__mbunDialect`; Node keeps the existing `nodeArgTypeError()`
  path, and Bun restores its compatibility wording. The release build passed
  in **60.34 seconds**.
- After the change, W83 measured **5/5 files green, 16 passed, 0 failed, 16
  ran, 9 expects**. The combined W82/W83 ten-file guard measured **10/10
  green, 29 passed, 0 failed, 29 ran**; Node's
  `test-path-parse-format.js` was independently **1/1 pass**. No upstream
  fixture changes and no full corpus were performed.
- Resource checkpoint around the build: approximately **43 GiB available
  memory, 48 MiB free swap, and 20 GiB free disk**. The run stayed within the
  serialized build lock and five-job bounded runner; no workspace-wide build
  was started.

### W84 Bun Node-path continuation

- A fresh five-file Bun path probe used **5 bounded jobs**, reused the existing
  Linux binary, and did not build. It measured **5/5 files green, 88 passed,
  0 failed, 89 ran, 382 expects**.
- The high-value `browserify` compatibility file was **52/52** and
  `matches-glob` was **31/31**. The same probe also covered path base
  properties, the long-path join guard, and zero-length strings; one skipped
  case in the path-property file is reflected by **88 passed / 89 ran**, not
  silently counted as a pass.
- This wave found no single runtime owner and required no source change or
  issue. No upstream fixture changes and no full corpus were performed; the
  next route remains a fresh near-green Node/Bun leaf with 3–5 bounded workers.

### W85 Bun Node API probe and Buffer ceiling fix

- The fresh five-file probe used **5 bounded jobs**. Before the fix it measured
  **3 green files, 1 all-skipped file, 147 passed, 2 failed, 150 ran, 3035
  expects**: `node:os` was **52/52**, `path.posix` and `path.win32` existence
  guards were **1/1** each, and the platform-specific relative-path file was
  all-skipped. `string_decoder` reached **93 passed / 2 failed / 95 ran**.
- Both failures were the same owner, not decoder logic: the child smoke tried
  `Buffer.allocUnsafe(2**31)` and `Buffer.allocUnsafe(2**31 + 16)`, but the Bun
  dialect still applied `0x7fffffff`, so the child exited before
  `StringDecoder.write()` or `.text()` could exercise their large-buffer
  guards. Direct reproduction returned `ERR_OUT_OF_RANGE` at the allocator
  boundary.
- Issue [#51](https://github.com/Sunrisepeak/mbun/issues/51) led to commit
  `03b22da`. The completion partition now selects Bun's 64-bit
  `K_MAX_LENGTH = 0x100000000` and `MAX_STRING_LENGTH = 0x7fffffff`, updates
  `Buffer.alloc*`, `Buffer.concat`, and the public module constants, and keeps
  the Node dialect's previous values unchanged. The retry build passed in
  **60.13 seconds**. A first attempt that edited the oversized `process_web`
  raw payload hit GCC16's constexpr string-length limit; that change was
  removed, keeping the final patch in the existing buffer completion owner.
- After the fix W85 measured **4 green files, 1 all-skipped file, 149 passed,
  0 failed, 150 ran, 3038 expects**. `string_decoder` is now **95/95** and
  `node:os` remains **52/52**. A separate Node direct guard still reports
  `MAX_LENGTH=2147483647`, `MAX_STRING_LENGTH=536870888`, and rejects
  `allocUnsafe(2**31)`, so the dialect boundary was verified without a large
  Node allocation. No upstream fixture changes and no full corpus were
  performed.
- Resource checkpoint after the build and bounded child runs: approximately
  **42 GiB available memory, 54 MiB free swap, and 20 GiB free disk**. No
  workspace-wide build or parallel build storm was started.

### W86 Buffer completion regression guard

- A focused five-file Bun guard reused the W85 binary with **5 bounded jobs**
  and no build. It measured **4 green files, 25 passed, 6 failed, 31 ran, 46
  expects**. `buffer-compare-bounds` (**13/13**),
  `buffer-from-encoding-leak` (**2/2**), `buffer-inspectmaxbytes` (**1/1**),
  and `buffer-utf16` (**1/1**) stayed green.
- `buffer-concat` reached **8/14**. Its six failures are not one #51 ceiling
  contract: they span the large-concat OOM error shape, resizable-buffer
  post-getter sizing, and detached TypedArray/ArrayBuffer propagation in
  `Bun.concatArrayBuffers`. The row is parked as a native concat/lifetime
  cluster; no source patch or new issue was opened.
- This is a guard result, not a new full Buffer-suite score. No upstream
  fixture changes, no full corpus, and no workspace-wide build were performed.

### W87 Bun Node-fs leaf confirmation

- A fresh five-file Bun filesystem probe reused the W85 binary with **5 bounded
  jobs** and no build. It measured **5/5 files green, 46 passed, 0 failed, 70
  ran, 92 expects**.
- `fs.glob` was **27/27**, `fs-path-length` **11/11**, Linux birthtime **5/5**,
  cp symlink target **2/2**, and recursive readdir error-leak **1/1**. The
  path-length file ran 35 cases and the readdir leak guard completed in the
  bounded runner without a timeout.
- This wave intentionally refreshed older inventory claims rather than treating
  them as current facts. All five remain green, no single source owner surfaced,
  and no issue or patch was needed. No upstream fixture changes, no full corpus,
  and no workspace-wide build were performed.

### W88 Bun Node-fs directory/Stats leaf confirmation

- A fresh five-file Bun filesystem probe reused the W85 binary with **5 bounded
  jobs** and no build. The dependency gate required the already-authorized
  missing-dependency measurement mode; this was a runner precondition, not a
  test failure. The probe measured **5/5 files green, 52 passed, 0 failed, 55
  ran, 138 expects**.
- `dir` was **23/23**, `fs-mkdir` **21/24**, async-iterator `writeFile` **2/2**,
  Stats constructor **3/3**, and Stats truncate **3/3**. This refreshes the
  older W70 filesystem result with current bounded evidence; no single source
  owner surfaced, no issue or patch was needed, and no upstream fixture was
  changed.
- No full corpus or workspace-wide build was performed. The next route remains
  a fresh one-owner Bun/Node leaf, with 3–5 bounded lanes while swap and disk
  headroom remain low.

### W89 Bun Node-inspector probe parked

- A fresh five-file Bun standard-module probe reused the W85 binary with **5
  bounded jobs** and no build. It measured **4/5 files green, 34 passed, 27
  failed, 64 ran, 131 expects**.
- `inspector.test` was **5/5**, diagnostics channel **6/9**, perf hooks **8/8**,
  and timers promises **4/4**. `inspector-profiler` reached **11 passed / 27
  failed / 38 ran**; the failures cover Session connected-state checks,
  profiler enable/start/stop return and state contracts, and unsupported-method
  errors.
- A narrow source check confirmed the current runtime advertises
  `process.features.inspector` as false and has no inspector/profiler
  implementation owner in the JSC builtins. This is a missing subsystem
  boundary, not a safe error-text or one-method patch; it is parked without a
  speculative issue or source change. No upstream fixture changes, full corpus,
  or workspace-wide build were performed.

### W90 Bun Node-fs/child_process leaf probe

- The first dispatch had one selector typo (`fs/fs-promises.test.js`); the
  runner classified it as a harness load error with no tests. It was not counted
  as runtime evidence. The corrected selector used the real
  `fs/promises.test.js` path and reran the same five-file wave with **5 bounded
  jobs** and no build.
- The authoritative corrected result was **4/5 files green, 39 passed, 12
  failed, 56 ran, 107 expects**. `child-process-exec` was **11/11**,
  `child-process-rlimit-nofile` **1/1**, `child-process-stdio` **7/7**, and
  Linux `fs-stat-seccomp` **3/3**.
- `fs/promises` reached **17 passed / 12 failed / 34 ran**. Its failures split
  across async stack frames, the internal stream loader, FileHandle close/
  in-flight lifetime, and AbortError message/shape. The current source layout
  does not expose one safe live owner for this mixed row, so no issue or patch
  was opened. No upstream fixture changes, full corpus, or workspace-wide
  build were performed; temporary selector and output data were removed.

### W91 Bun Node-net leaf probe

- A fresh five-file Bun net probe reused the W85 binary with **5 bounded jobs**
  and no build. It measured **3 green files, 5 passed, 1 failed, 7 ran, 15
  expects**; one additional file was classified `no-tests`.
- `double-connect` was **1/1**, `node-net-allowHalfOpen` **2/2**, and
  `socket-reconnect-live` **2/2**. The `handle-leak` fixture is a stress
  entrypoint rather than a Bun test file; it completed its 100,000-connection
  leak exercise but is not counted as green coverage.
- `connect-autoselectfamily-stale-timer` skipped its macOS-only case and timed
  out the Linux destroy-while-pending case after **5 seconds**. The fixture
  never reached its `OK` marker; the source path has no bounded Happy-Eyeballs
  attempt-timer lifecycle to patch narrowly, so this remains a net liveness
  boundary with no speculative issue or source change. No upstream fixture
  changes, full corpus, or workspace-wide build were performed.

### W92 fs/promises AbortError message fix

- W90's red `fs/promises` row was reduced to one owner: the live
  `fsAbortErr()` helper emitted `The operation was aborted` without Node's
  terminal period. Issue [#52](https://github.com/Sunrisepeak/mbun/issues/52)
  captured the red contract; commit `84f7d59` changes only that message and
  preserves `name`, `code`, and `cause`.
- A fresh release build completed under the serialized build lock. The focused
  Bun rerun used **5 bounded jobs**: `fs/promises` moved from **17 passed / 12
  failed / 34 ran** to **24 passed / 5 failed / 34 ran**, and the four W90
  child_process/fs guards remained green. Aggregate focused result: **4/5
  files green, 46 passed, 5 failed, 56 ran, 114 expects**.
- The seven AbortError message/shape cases all turned green. The five remaining
  failures are independent async-stack, internal-stream-loader,
  FileHandle-lifetime, and async-iterator abort-assertion owners; no mixed
  follow-up was attempted. Direct smoke reports the corrected message in both
  Node and Bun dialects with `AbortError` and `ABORT_ERR`.
- Node's bounded guard passed the two FileHandle abort files; the third
  readFile file remains red only at its unrelated zero-byte-liar child fixture
  assertion. No upstream fixture changes, full corpus, or workspace-wide build
  were performed.

### W93 Node fs/promises FileHandle leaf confirmation

- A fresh Node corpus probe reused the W92 release binary with **5 bounded
  jobs** and no build. All **5/5 files passed**: FileHandle `chmod`, `stat`,
  `truncate`, `write`, and `sync`.
- This is a post-fix Node-side guard for the adjacent fs/promises surface. It
  found no new source owner and required no fixture change or issue. No full
  corpus or workspace-wide build was performed; temporary selector and output
  data were removed.

### W94 Bun HTTP/2/Worker staged probe before fix

- A fresh five-file Bun probe used **5 bounded jobs** and the existing binary;
  it measured **4/5 files green, 7 passed, 1 failed, 8 ran, 6 expects**.
- HTTP/2 late-RST (**2/2**) and streams-rehash (**3/3**) were green, as were
  Worker SharedArrayBuffer (**1/1**) and transfer-terminate (**1/1**). The
  single red file was the reserved-push DATA refusal case, which timed out
  before observing `RST_STREAM(STREAM_CLOSED)`.
- Narrow source inspection showed the client parser created the pushed stream
  but forwarded DATA before response HEADERS. Issue [#53](https://github.com/Sunrisepeak/mbun/issues/53)
  captured that one state-machine owner; no mixed HTTP/2 patch was attempted.

### W95 Bun HTTP/2/Worker staged regression after #53

- Commit `fa7b12b` adds only the reserved-push guard in the client DATA path:
  before response HEADERS, it sends `STREAM_CLOSED`, closes the stream, and
  keeps the session available for later frames.
- The serialized fresh build completed. The same five-file probe with **5
  bounded jobs** measured **5/5 files green, 8 passed, 0 failed, 8 ran, 8
  expects**. The former push-refusal timeout is now **1/1**, while late-RST,
  streams-rehash, SharedArrayBuffer, and transfer-terminate remain green.
- No upstream fixture changes, full corpus, or workspace-wide build were
  performed; temporary selectors and outputs were removed.

### W59 Node buffer leaf sample

- W59 使用 Node corpus runner 的默认 bounded profile、**3 jobs**。首批五个文件为
  **4/5 pass**，第二批五个文件为 **5/5 pass**，第三批五个文件在修复前为
  **4/5 pass**；因此完整样本修复前为 **13/15 pass、2 failed**。
- Issue [#43](https://github.com/Sunrisepeak/mbun/issues/43) 负责同一
  `Buffer.prototype.fill` 入口的三个 Node contract：hex 奇数/非法字符的
  `ERR_INVALID_ARG_VALUE`、非字符串 encoding 的 `ERR_INVALID_ARG_TYPE`，以及
  伪造 `length` 时的 `ERR_BUFFER_OUT_OF_BOUNDS`。修复仅位于
  `modules/jsc/src/builtins/node_buffer_extra.cppm`，上游测试保持只读。
- root release build 成功，耗时 **60.70 秒**；focused fill + 9 guards 为
  **10/10 files pass**，完整 W59 复测为 **14/15 files pass**。唯一失败仍是
  `test-buffer-constants.js` 的通用 JSC String capacity 边界，不归入 Buffer.fill
  owner。未跑全量 corpus，未启动 workspace-wide build。

### W58 JSON5/YAML parser sample

- W58 复用已有 fresh binary，默认 **4G/512、3 jobs**，无构建、无全量 corpus。三个
  parser 文件全绿：JSON5 扩展 **321/321**、JSON5 官方 suite **113/113**、YAML
  block-scalar matrix **1084/1084**。`import-attributes` 为 **3/12**，9 个失败
  跨无扩展 JS/TS loader、JSON/JSONC/TOML/YAML loader、tsconfig JSONC 识别以及
  wasm/不存在模块处理，不能归并为一个安全 owner。
- W58 合计 **4 files、1521/1530 tests passed、9 failed、1809 expects、3/4 files
  green**。JSON5/YAML 结果作为新的高收益绿色覆盖记录；import-attributes 停车，
  下一任务仍需 fresh bounded measurement 证明单 owner 后再建 issue。

### W57 fresh Bun built-in probes

- W57 使用默认 **4G/512、3 jobs**，无构建、无全量 corpus。第一批五文件为
  `ini/ini`、`jsonl/jsonl-parse`、`jsonc/jsonc`、Markdown heading IDs 和
  `md/gfm-compat`：前四个分别为 **62/62、269/269、43/43、17/17**，GFM 为
  **47/62**，15 个失败。失败证据分成 table interruption/column-count/escaping、
  autolink 特殊字符、单波浪线删除线和 entity-like suffix，至少四个 owner，未建
  混合修复 issue。
- 第二批 cookie 四文件与 `cron/cron-parse` 全绿：**5/5 files、138/138 tests、0
  failed、667 expects**。两批合计 **10 files、576/591 tests passed、15 failed、5479
  expects**，其中 **9/10 files green**；失败全部来自 GFM，cookie/cron/ini/JSON/heading
  结果可作为下一轮 guard 或已交付覆盖数据。
- 第三批 util probe 使用同一 profile 测得 base64url **5/5**、escapeHTML **10/10**、
  escapeRegExp **2/2**、which **5/5**；`stripANSI` 为 **284/296**，12 failed。失败
  分成 malformed CSI/OSC/C1、Unicode/C1 边界和 large-input 性能断言多个 owner。
- 三批独立选集合计 **15 files、882/909 tests passed、27 failed、7451 expects**，其中
  **13/15 files green**。结论：GFM 与 stripANSI 都停车；下一任务继续从 fresh
  bounded measurement 选单一 Bun/Node near-green row，不因绿色叶子文件而
  speculative 改动 parser 或 ANSI scanner。

### W56 Bun.Glob path-boundary owner

- W56 先纠正了一次候选路径选择错误：错误的 `compat/bun/...` 前缀只触发了
  harness 的 no-test/load-error 分类，未计入覆盖数据。修正为真实 vendored 路径后，
  `cli/install/semver`、`glob/match`、`glob/proto`、`util/toUTF16Alloc` 四个文件先得
  到 **4/4 files、58/58 tests、0 failed、3261 expects**；随后加入
  `glob/path-length`，初始为 **1/6、5 failed**，失败集中在同一条路径边界 owner。
- Issue [#42](https://github.com/Sunrisepeak/mbun/issues/42) 和设计记录
  `.agents/docs/20260801-bun-glob-path-boundary-design.md` 先固定了契约：复用
  `mbun.platform.path` 的 host policy，在 pattern、目录下降以及
  `directory_entry` status/iterator 返回 `ENAMETOOLONG` 时传递错误；matched file 的
  逻辑路径仍按原生测试要求可返回，并保留 matcher、symlink-cycle 和 only-files 语义。
- `d8d8082` 在 `glob.cppm` 增加可选 scan error-code 输出和目录边界传播，在 JSC
  callback 使用已有 Node-shaped fs error helper，并补齐 only-files fast path 的
  `absolute` 结果。glob 单元目标为 **1497 checks、0 failures**；root release build
  约 **59.3 秒**。
- 最终 bounded lane 使用默认 **4G/512、3 jobs**，9 个真实文件为 **9/9 files
  green、193/193 tests、0 failed、3856 expects**：`glob/path-length` 已从 **1/6**
  提升到 **6/6**，四条既有 guards 仍为 **4/4 files、128/128 tests、0 failed、565
  expects**。未跑全量 corpus，未做 workspace-wide build。

### W55 fresh inventory refresh and Bun CSS triage

- W55 首先复测了 inventory 指向的 Node fs owner：FileHandle
  `pull/pullSync/writer/aggregate-errors/close-errors/op-errors` **6/6 pass**；随后
  flush、AbortSignal 与 WHATWG URL 组合的 `append-file-flush`、`write-file-flush`、
  `readfile`、`write-file`、`whatwg-url` **5/5 pass**。两组共 **11/11 pass**，说明旧
  inventory 条目已 stale，本轮没有为它们创建 issue 或 source patch。
- 为验证 Bun 侧最高价值的可单层入口，测量了 5 个真实引用 `cssInternals` 的文件，
  使用默认 **4G/512、5 jobs**：**1/5 files green、6/15 tests passed、9 failed、
  30 expects**。`custom-pseudo-ident-escape` **4/4** 已绿；其余失败分别落在
  缺失 `cssInternals._test`、angle 非有限数值序列化、attribute-selector parser、
  nested-selector expansion，不能由一个安全 wrapper 闭合。
- 结论：不把旧 inventory 当作当前事实，不对 CSS `~12-file` cluster 做跨 owner
  混修；下一轮必须从 fresh bounded measurement 选单一 owner。W55 无构建、无全量
  corpus，资源策略继续保持 3–5 jobs，并在 swap/disk 低水位时只做小型 probe。

## Next route

1. Keep the native-syntax compatibility gate limited to the two measured
   optimization controls; evaluate additional V8 intrinsics only from their
   own failing corpus evidence.
2. Keep callbackify stack shape, util.format's constructor-name mismatch, and
   assertion source-position parked behind their generic runtime boundaries.
   Treat the VM parser-message mismatch the same way; use the next measured
   Node or Bun actionable row only after its owner and expected contract are
   identified.
3. Keep test-v8 profiler/queryObjects and broad VM wording changes parked until
   ownership is clear.
4. Keep the W56 Bun.Glob, W57 green builtin/util slices, W58 JSON5/YAML parser
   sample, and W59 Node buffer leaves closed unless a new minimal reproduction
   reopens them. Keep GFM, stripANSI, import-attributes, and buffer constants
   parked behind their generic or multiple owners; prioritize the next one-owner
   Bun row over zlib's native-handle cluster and test-runner's multi-owner boundary.
5. Keep the fixed W51 final-read ordering, W52 file-backed stdin, W53 stdout
   disturbed/reject, and W54 conversion-helper brand owners closed; reopen only
   with a new minimal reproduction.
6. Treat old inventory entries as hypotheses only: refresh the named files before
   dispatching a fix. Select the next task only from a fresh one-owner Bun/Node
   near-green row; keep 3–5 bounded lanes, record per-file pass/fail/expect counts,
   and defer full-corpus runs and broad builds while swap or disk headroom remains
   low.

No local absolute paths, user names, host names, credentials, private URLs, or
machine-specific identifiers belong in future comments, commits, PR text, or
copied logs; use placeholders when examples need them.
