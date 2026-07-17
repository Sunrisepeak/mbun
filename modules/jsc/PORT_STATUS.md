# JSC API seams port status

This checkpoint preserves the engine-neutral part of three Bun integration
surfaces. It is a translation checkpoint, not a claim of complete runtime or
JavaScript compatibility.

## Translated in this checkpoint

- `mbun.jsc.node_fallbacks`: the 24-byte virtual path prefix, 23-module lookup
  table, source keys, ESM marker, polyfill version, and side-effect metadata
  from Bun's Rust and Zig resolver implementations.
- `mbun.jsc.bun_api.*`: missing-argument-as-`undefined`, static API descriptors,
  and the `Thrown` / `OutOfMemory` / `Terminated` host-result distinction from
  Bun's Rust `host_fn` and Rust/Zig `CallFrame` implementations.
- `mbun.jsc.web_api`: Request/Response owned body state, clone/body-used
  invariants, Bun's known-method normalization, header validation, and Bun's
  `101` or `[200, 599]` response status contract.
- `mbun.jsc.web_streams.queue`: the JSC-free queue-with-sizes algorithms,
  including finite non-negative size validation, FIFO peek/dequeue, rounded
  total-size clamping, and reset.
- `mbun.jsc.web_streams.state`: engine-neutral readable and writable stream
  state projections. Writable close remains an operation flag while stream
  state stays `writable`; `erroring` and `errored` remain distinct.

## DEFERRED

- Loading/generated embedding of the Node fallback JavaScript payloads and
  bundler resolver installation.
- JSC `CallFrame` register decoding, `JSValue` coercion, generated class
  registration, platform-specific host-call ABI, exception-scope validation,
  and OOM throwing through `JSGlobalObject`.
- WebCore/JSC wrappers; Web IDL coercion; `ReadableStream` teeing; Promise,
  Blob, FormData, AbortSignal, CookieMap, redirect/cache/mode fields; global
  Request/Response constructor installation; and the full Bun web test corpus.
- Complete WHATWG Streams controllers and algorithms, JSValue/GC ownership,
  Promise request queues, reader/writer locking, byte/BYOB streams, tee/pipe,
  desired-size and backpressure propagation, TransformStream coupling, and
  JSC/WebCore constructor/prototype installation.

## Build note

`modules/jsc/mcpp.toml` already contains the required member-local index:
`mbun = { path = "../../mcpp" }`.

The first GCC 16.1.0 `mcpp test` resolved that index, compiled all 15 test
targets, and finished with 14 passing targets plus one failure in the new
`test_node_fallbacks`: direct module lookup passed, while virtual-path lookup
returned empty. Replacing pointer-identity assertions exposed that the failure
was unchanged after replacing `string_view::starts_with` with a direct byte
comparison. The exported lookup helpers were therefore moved out of constant
evaluation so table access and nested lookup execute in the module object,
instead of through GCC's imported BMI expression. The follow-up result is
GCC 16.1.0 `mcpp test`: 15 test targets passed, 0 failed. The three new direct
import tests reported Bun API 13 checks, Node fallbacks 12 checks, and Web API
27 checks, all with 0 failures.

For the Web Streams checkpoint, GCC 16.1.0 `mcpp test` again resolved the
member-local index and then started downloading `mbun.jsc-prebuilt v20260706`
(293 MB). The run was intentionally stopped during that dependency download;
it did not reach C++ compilation, so this checkpoint records build information
and does not claim the new direct-import test is green.

## Shell template-tag checkpoint

- `Bun.$` is installed as a real tagged-template runtime backed by the translated
  `mbun.shell` parser/template compiler and the existing asynchronous `Bun.spawn`
  process layer. It implements `ShellPromise`, `ShellOutput`, `ShellError`,
  `nothrow`/`throws`, interpolation escaping, `cwd`, `env`, and output helpers.
- Template interpolation context comes from Bun-style parser markers in the
  recursive shell lexer, so nested command substitutions cannot reinterpret JS
  values as operators. The process-backed Windows path is explicitly unsupported
  and fails before execution instead of passing POSIX quoting to `cmd.exe`.
- The shell member's parser, execution seam, and template tests pass with GCC 16
  and LLVM 22. The GCC JSC suite passes all 19 targets, including focused shell
  coverage for nonzero exits, throwing, interpolation, output, environment,
  working directory, chmod, and relative execution.
- `compat/bun/test/js/bun/util/which.test.ts` passes all five Linux tests with zero skips
  when the built executable is exposed as `bun` on `PATH`, matching the upstream
  test's executable-name precondition.
- LLVM 22 reaches the existing JSC `test_runner.cppm` aligned-allocation
  ambiguity before running the JSC suite; this compiler-specific baseline issue
  is outside the shell partitions. The shell member itself is green on LLVM 22.
