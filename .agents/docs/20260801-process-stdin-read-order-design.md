# process.stdin final read/end ordering

## Scope

Track the Linux Bun-native corpus failure in `process/process-stdin.test.ts`:
`read(3)` returns the final buffered bytes, but the `end` listener can observe
the caller's result array before that final return value has been recorded.

## Reproduction

Using the existing fresh mbun binary and a bounded stdin pipe:

```text
input: abcdefgh
read size: 3
observed end payload: ["abc", "def"]
returned reads: ["abc", "def", "gh"]
```

The same source path with a `readable` listener reports all three chunks before
`end`, so the defect is specifically the synchronous end emission from the
last `read(size)` call when EOF has already been observed.

## Root cause

`bootstrap.cppm` calls `emitEnd()` before returning the final buffer from
`process.stdin.read(size)`. JavaScript callers record the returned chunk only
after the function returns, so the event can observe an incomplete consumer
state.

## Minimal fix contract

- Preserve the returned final buffer and its byte count.
- Schedule `end` after the current `read()` call returns.
- Keep EOF, readable-mode buffering, and close behavior unchanged.
- Do not change the Bun `ReadableStream` stdin adapter or child-process stdin.

## Acceptance

- The existing upstream `process/process-stdin.test.ts` passes its explicit
  `read(n)` case.
- The focused pause/readable stdin probes remain green.
- The four-file W48 probe is rerun; no full Bun/Node corpus or parallel build.

## Resource guard

Use one serial build/test lane only. Keep test timeout bounded and record the
resource profile because swap headroom is low and root disk headroom is tight.
