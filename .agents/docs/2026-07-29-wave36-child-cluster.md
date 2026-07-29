# Wave 36 child/cluster static assessment

Baseline: `275d9c5d533a160f0840cf49fda5f5a965ae4c00`.

Scope source commit: `916570b fix(child-process): normalize spawn edge contracts`.
The compatibility inputs under `compat/` were read only. Per the requested
time box, no build or runtime test was started; “candidate” below is a static
expectation, never an asserted green result.

## Changes

- Normalize public child-process options into null-prototype own-property
  snapshots. Inherited `cwd`, `shell`, uid/gid, and related values can no
  longer leak from `Object.prototype` into async or sync spawning.
- Set a child stdin stream's observable `readable` flag to `false`.
- Give deferred async spawn failures a negative libuv errno, so ENOENT round
  trips through `util.getSystemErrorName`.
- Carry `options.argv0` into the synchronous native exec argv and expose the
  invoked argv0 separately from the real executable path in a child runtime.
- Reject non-buffer/non-string `spawnSync({ input })` values synchronously.

## Static worklist coverage

| Compatibility file | Static disposition | Basis |
| --- | --- | --- |
| `test-child-process-prototype-tampering.mjs` | expected candidate (new) | Each public spawn normalization path reads only own options. |
| `test-child-process-spawn-argv0.js` | expected candidate (new) | Sync native argv[0] and child `process.argv0` both preserve `argv0`. |
| `test-child-process-spawn-error.js` | expected candidate (new) | Async ENOENT exposes a negative UV errno and existing path/syscall fields. |
| `test-child-process-stdin.js` | expected candidate (new) | Parent-side stdin is explicitly write-only (`readable === false`). |
| `test-child-process-spawnsync-input.js` | partial candidate (new) | Invalid scalar input now throws the required TypeError; binary transport remains runtime-unverified. |
| `test-child-process-cwd.js` | baseline candidate | Existing path/URL conversion and deferred missing-cwd error paths already match the checked contract. |
| `test-child-process-spawnsync-shell.js` | baseline candidate | Existing shell wrapping and inherited-environment logic cover the static route. |

This yields four new complete static candidates, meeting the three-candidate
time-box goal without claiming any unrun file is green.

## Deferred backend-dependent cases

- `test-child-process-fork-net.js`: SCM_RIGHTS socket handoff, IPC ordering, and
  delayed close behavior.
- `test-child-process-http-socket-leak.js`: transferred HTTP socket/parser
  lifecycle.
- `test-child-process-spawn-windows-batch-file.js`: platform-specific CreateProcess
  behavior.
- `test-child-process-advanced-serialization-largebuffer.js`: 8 MiB advanced IPC
  framing and backpressure.
- `test-child-process-destroy.js`: process-group signal and close-event timing.

`changelog.md` is intentionally unchanged: this wave has no runtime evidence.

## Static verification performed

```text
git diff --check
{ selected complete child-process payload section; } | node --check
clang-format --dry-run --Werror --style=file --lines=... runtime/process_base.inc
clang-format --dry-run --Werror --style=file --lines=... runtime/bindings_install.inc
```

All listed checks exited successfully. No build or compatibility command was
run by instruction.
