# Bun.spawn ReadableStream stdin — minimal design

## Goal

Make `Bun.spawn({ stdin: ReadableStream })` use the existing asynchronous
`spawnEx`/`__mbun_io_tick` path on Linux. The current dispatch only recognizes
strings, byte views, ArrayBuffers, Blobs, and keyword stdin values; a
`ReadableStream` falls through to the synchronous fallback and the child
receives no input.

The acceptance target is the vendored Bun stream-input contract. The upstream
tests remain read-only and are the source of truth.

## Evidence and scope

The bounded four-file probe measured:

- `spawn-stdin-readable-stream.test.ts`: **7 passed, 21 failed, 30 ran**;
  ordinary data, delayed pulls, byte chunks, stream errors, cancellation, and
  early-child-exit cases all report the missing stream-input boundary.
- `spawn-streaming-stdout.test.ts`: **1 passed, 0 failed, 211 expects**;
  this is the required adjacent regression lane.
- `spawnSync.test.ts`: **6 passed, 7 failed** across timeout, fixture/optimization,
  and uid/gid owners; it is explicitly out of scope for this change.
- `spawn.test.ts`: timed out at the bounded file limit and is not used as a
  pass/fail signal for this owner.

## Design

1. Detect a real `ReadableStream` with the already-installed
   `G.__mbunStreams.isReadableStream` predicate. Before creating a child,
   reject locked or disturbed streams with the Bun-facing stdin error shape.
2. Create the child through the existing `spawnAsyncBun(..., { stdin: "pipe" })`
   path. Do not alter string, byte-view, ArrayBuffer, Blob, keyword, or
   `stdin: "pipe"` dispatch.
3. Acquire the stream's default reader and pump one `read()` result at a time
   into the existing non-blocking stdin queue. Convert strings and supported
   byte views through the existing `anyToU8` helper; close the sink only after
   `{ done: true }`.
4. Race the read loop with the child exit. On child termination, cancel the
   reader and release it so a pull source cannot keep the parent alive. Mark
   the pump promise handled; an input error must not become an unhandled
   rejection after the child has already closed.
5. Keep the implementation in the JS payload because the behavior is Web
   Streams reader/prototype/Promise protocol glue. The reader pump lives in a
   small `bun_spawn_stream.cppm` payload immediately after `process_web.cppm`:
   the latter is already near GCC's 262144-character constexpr string limit.
   The two payloads are concatenated into the same IIFE and lexical scope.
   Native process I/O stays in `proc.spawnEx` and `__mbun_io_tick`; no second
   native pump is introduced.

## Error and lifecycle behavior

- A locked or already-disturbed stream fails synchronously before spawning.
- A source that closes normally ends child stdin and preserves exact bytes,
  including NUL bytes and mixed string/typed-array chunks.
- A source error preserves data already delivered, closes the child input, and
  is handled by the pump so it does not surface as an unhandled rejection.
- Child exit/kill cancels the reader once and releases it; later source pulls
  must not write to a closed descriptor or pin the event loop.
- AbortSignal behavior remains owned by `spawnAsyncBun`; the stream pump only
  observes the resulting child exit.

## Verification plan

After the smallest source change, run through the bounded runner with four
jobs, but select only:

1. `spawn/spawn-stdin-readable-stream.test.ts` (primary red→green signal);
2. `spawn/spawn-streaming-stdout.test.ts` (adjacent stream regression);
3. one already-green process lane if the focused run is green, without starting
   a full corpus pass.

Because this changes `process_web.cppm`, one incremental Linux build is needed
before runtime verification. The build starts only after the resource gate is
checked again; no parallel build is allowed. The target record is the focused
before/after count, not a claim about the entire Bun corpus.

## Non-goals

- `spawnSync` timeout=0, memfd/optimization fixtures, uid/gid error shape;
- the broad `spawn.test.ts` timeout;
- async-iterable stdin support beyond what the existing stream contract needs;
- changes to `compat/` test files or full-corpus measurement.
