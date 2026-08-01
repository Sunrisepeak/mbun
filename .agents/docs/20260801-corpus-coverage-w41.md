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
4. Re-measure the adjacent path sample only if it can be done without a new
   broad build; otherwise prioritize the next one-owner Bun row over zlib's
   native-handle cluster and test-runner's multi-owner boundary.
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
