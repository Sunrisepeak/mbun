# Bun.spawn file-backed stdin owner

## Scope

Track the Linux Bun-native `process/process-stdin.test.ts` failure where
`Bun.spawn({ stdin: Bun.file(...) })` exposes the wrong child stdin shape.

## Root cause

The `Bun.file()` Blob branch previously routed through the live pipe path and
copied bytes into an anonymous pipe. Node/Bun file-backed stdin uses a regular
file descriptor instead. The bootstrap therefore saw a pipe and installed
`process.stdin.ref/unref`, while a regular-file stdin must not expose those
methods.

## Minimal fix contract

- Detect only regular-file `Bun.file()` inputs and pass an opened fd as stdio.
- Close the parent-side fd immediately after the child spawn boundary duplicates
  it; the child owns its fd 0.
- Keep generic Blob, byte, ReadableStream, async-iterable, and keyword stdin
  paths unchanged.
- Install stdin `ref/unref` only when fd 0 is not a regular file.

## Acceptance

- The existing file-backed stdin case passes while the adjacent pipe case stays
  green.
- The focused stdin file improves from 12/14 to 13/14; the remaining failure is
  the separately parked stdout WebStream disturbed/reject owner.
- Four existing green guards remain 128/128 with zero failures.
- The W52 four-file bounded probe is rerun; no full Bun/Node corpus.

## Resource guard

Use the root-only release build and bounded runner profile (4G/512). Stop before
workspace-wide compilation if swap or disk headroom approaches the recorded
Linux threshold.
