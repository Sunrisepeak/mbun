# Wave32 Node Worker Tail

Baseline: `0ae22f2`; branch: `codex/wave32-node-tail`.

## Static expected greens

| Node corpus file | Source change | Expected result |
| --- | --- | --- |
| `test-worker-broadcastchannel-wpt.js` | Drain each receiver queue in channel creation order. | Green: preserves Node's port-order delivery and close-during-drain behavior. |
| `test-worker-cli-options.js` | Reject an explicitly supplied `--expose-gc` with `ERR_WORKER_INVALID_EXEC_ARGV`. | Green: inherited flags and Node options retain their existing path. |
| `test-worker-error-stack-getter-throws.js` | Preserve a failed `Error.prepareStackTrace` read as an undefined parent error stack. | Green: error name and message still cross the worker IPC boundary. |
| `test-worker-data-url.js` | Classify JavaScript `data:` worker URLs, reject non-JavaScript MIME, and return status 13 for an unsettled top-level await. | Green: valid module sources retain the module route. |

No build or runtime test was run, per the Wave32 no-build/no-test constraint. These are static estimates, not executed corpus results.

## Deferred backend work

| Node corpus file | Blocking capability |
| --- | --- |
| `test-worker-terminate-source-map.js` | True shared `SharedArrayBuffer` memory plus hard worker teardown. |
| `test-worker-beforeexit-throw-exit.js` | Shared `SharedArrayBuffer`/Atomics worker memory. |
| `test-worker-cwd-race-condition.js` | Shared `SharedArrayBuffer`/Atomics synchronization. |
| `test-worker-http2-generic-streams-terminate.js` | Shared counters and mid-operation worker termination. |
| `test-worker-message-channel-sharedarraybuffer.js` | Shared heap backing across child-process workers. |
| `test-worker-message-not-serializable.js` | Node internal worker IPC serialization-error envelope. |
| `test-worker-message-port-drain.js` | Child-process stdout drain and message-port lifecycle integration. |
| `test-worker-message-port-transfer-fake-js-transferable-internal.js` | FileHandle transferable ABI and `messageerror` semantics. |

## Static checks

- `git diff --check` passed.
- `clang-format --dry-run --Werror` passed for the two changed embedded-JS C++ modules.
- Extracted embedded JavaScript from both changed modules passed `node --check`.
- The edited `engine.inc` fragment matches `clang-format` output. A whole-file
  `clang-format --dry-run --Werror modules/jsc/src/runtime/engine.inc` fails at
  existing violations beginning at line 70, so it is not a new clean-file
  signal for this change.
