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

## Next route

1. Keep the native-syntax compatibility gate limited to the two measured
   optimization controls; evaluate additional V8 intrinsics only from their
   own failing corpus evidence.
2. Evaluate the already measured `test-util-callbackify.js` stack-shape mismatch
   as the next single-file candidate; keep util format/types/inspect semantics and
   bootstrap assertion source extraction as separate lanes.
3. Keep test-v8 profiler/queryObjects and broad VM wording changes parked until
   ownership is clear.

No local absolute paths, user names, host names, credentials, private URLs, or
machine-specific identifiers belong in future comments, commits, PR text, or
copied logs; use placeholders when examples need them.
