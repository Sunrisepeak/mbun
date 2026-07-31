# Bun.spawn ReadableStream stdin Implementation Plan

> **For agentic workers:** Execute the tasks in this plan inline with a fresh red/green checkpoint after each source change.

**Goal:** Route `Bun.spawn({ stdin: ReadableStream })` through the existing asynchronous pipe path so stream chunks reach the child with correct close, error, and early-exit lifecycle behavior.

**Architecture:** Keep native process creation and non-blocking fd flushing in `proc.spawnEx` and `__mbun_io_tick`. Add one JS protocol adapter beside `spawnAsyncBun` that acquires a Web Streams reader, forwards each chunk to the existing stdin sink, and cancels/releases the reader when the child closes. Dispatch only real `ReadableStream` values to this adapter; existing string, byte, Blob, keyword, and `stdin: "pipe"` paths remain unchanged.

**Tech Stack:** C++26 module raw-string builtin payload, JSC Web Streams shim, `tools/integration/bun_corpus_runner.py`, vendored Bun tests.

## Global Constraints

- Treat `compat/bun/test/**` as read-only upstream input.
- Run child-spawning tests only through the bounded integration runner.
- Use one incremental Linux build at most; do not start a full corpus run.
- Keep the implementation in `modules/jsc/src/builtins/process_web.cppm`; do not add a second native pump.
- Commit one development item with `git commit -s` and `Co-authored-by: Codex <codex@openai.com>`.
- Do not place local absolute paths, usernames, hostnames, credentials, private URLs, environment values, or machine identifiers in tracked text or PR material.

---

### Task 1: Reconfirm the upstream red lane

**Files:**
- Read-only test: `compat/bun/test/js/bun/spawn/spawn-stdin-readable-stream.test.ts`
- Read-only runner: `tools/integration/bun_corpus_runner.py`

**Interfaces:**
- Consumes: existing binary and the four-file Wave 46 result.
- Produces: a focused red baseline for the source change.

- [x] **Step 1: Run the focused red test through the bounded runner.**

Run the existing four-file probe with `spawn-stdin-readable-stream.test.ts`,
`spawn-streaming-stdout.test.ts`, `spawnSync.test.ts`, and `spawn.test.ts`, using
four jobs and a 30-second per-file bound. Record only root-relative test names,
counts, and classifications.

- [x] **Step 2: Confirm the failure is functional.**

The stream file must report empty child output for the basic/chunk cases, while
the streaming-stdout file remains green. A timeout is recorded separately and
does not become an actionable source failure.

### Task 2: Add the minimal stream-input adapter

**Files:**
- Modify: `modules/jsc/src/builtins/process_web.cppm` near `spawnAsyncBun` and `Bun.spawn`

**Interfaces:**
- Consumes: `spawnAsyncBun(cmd, opts)`, `anyToU8(value)`,
  `G.__mbunStreams.isReadableStream(value)`, and `proc.exited`.
- Produces: `pumpBunReadableStdin(proc, stream)`; it returns no public value and
  owns a handled internal promise.

- [ ] **Step 1: Validate the stream before creating the child.**

Use `G.__mbunStreams.isReadableStream(value)` to identify the source. If
`value.locked` is true, throw a `TypeError` synchronously. If
`G.__mbunStreams.isDisturbed(value)` is true, throw exactly
`'stdin' ReadableStream has already been used` so the existing upstream
assertion can observe the Bun-facing error.

- [ ] **Step 2: Implement the handled reader pump.**

Acquire `stream.getReader()`, read one result at a time, and call the existing
`proc.stdin.write(anyToU8(value))`. Stop on `done`, then call
`proc.stdin.end()`. Catch source/read/write failures, end the sink, and attach a
`.catch(() => {})` to the internal pump promise so a late pipe error cannot
  become an unhandled rejection.

- [ ] **Step 3: Tie the reader to child lifecycle.**

Race each reader read against `proc.exited`. When the child closes, call
`reader.cancel()` once, handle a rejected cancel promise, and release the
reader in `finally`. Do not cancel after ordinary EOF; this preserves a source's
normal close semantics while preventing a pending pull from keeping the parent
alive after child termination.

- [ ] **Step 4: Add only the new dispatch branch.**

Before the existing byte and Blob branches in `Bun.spawn`, validate a detected
ReadableStream, create `spawnAsyncBun(s.cmd, { ...s.opts, stdin: "pipe" })`,
start the pump, and return the process. Leave all other branches byte-for-byte
unchanged unless the compiler requires a local helper name adjustment.

### Task 3: Build and verify the focused green lane

**Files:**
- Read-only tests: `compat/bun/test/js/bun/spawn/spawn-stdin-readable-stream.test.ts`,
  `compat/bun/test/js/bun/spawn/spawn-streaming-stdout.test.ts`
- Evidence output: temporary bounded-run directory outside the repository

**Interfaces:**
- Consumes: the Task 2 source change and the Linux resource gate.
- Produces: fresh build exit status, focused pass/fail counts, and regression
  counts for the adjacent stream lane.

- [ ] **Step 1: Recheck resource headroom.**

Do not build if the disk or swap gate is below the safe threshold. If the gate
is acceptable, run one incremental Linux build and select the newest binary by
mtime; do not run concurrent builds.

- [ ] **Step 2: Run the two-file bounded verification.**

Run the stream-input file and streaming-stdout file through
`bun_corpus_runner.py` with four jobs and a 30-second file bound. Read the
machine summary and each failure line; do not print whole logs into tracked
files or PR comments.

- [ ] **Step 3: If the stream file is not green, classify the next owner.**

Use the exact failing assertion to distinguish reader lifecycle, child-exit
cancellation, or an unrelated async-iterable case. Make at most one additional
source hypothesis in this commit; otherwise stop the commit at the measured
partial gain and park the remaining owner.

### Task 4: Record, checkpoint, and publish

**Files:**
- Modify: `changelog.md`
- Modify: `.agents/docs/20260801-corpus-coverage-w41.md`
- Source commit: `modules/jsc/src/builtins/process_web.cppm`

**Interfaces:**
- Consumes: fresh Task 3 counts and build result.
- Produces: one additive source checkpoint, sanitized changelog evidence, and
  a verified PR comment with the next route.

- [ ] **Step 1: Record before/after numbers.**

Add a dated entry with the focused file counts, adjacent regression counts,
build status, and explicit scope limits. Do not claim full Bun or Node corpus
coverage.

- [ ] **Step 2: Commit only this development item.**

Stage the source and documentation files explicitly, run `git diff --check`,
commit with a conventional `fix(compat): ...` subject and the required signoff
and co-author trailer, then inspect the commit diff.

- [ ] **Step 3: Push normally and verify the PR state.**

Push the branch without force or history rewriting. Add one sanitized PR comment
with the exact focused counts and next route, then verify the comment through the
GitHub API. Report CI as pending unless a fresh check conclusion says otherwise.
