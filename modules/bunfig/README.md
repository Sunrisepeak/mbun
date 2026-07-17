# mbun.bunfig

Semantic parser and data model for `bunfig.toml`. The TOML value tree comes from
`mbun.toml`; this layer applies bun's type checks, command gates and enum
coercions on top. The model mirrors the fields consumed by the Rust rewrite and
the Zig parser (`.mbun/bun-zig-src/src/cli/bunfig.zig`), and the parser phases
preserve the upstream command gates and validation order. Covered segments:
top-level (`logLevel`/`define`/`origin`/`env`/`preload`/`telemetry`/`smol`/
`serve.port`), `[test]`, `[install]` (registry/scopes/cache/lockfile/ca/...),
`[run]`, `[console]`, `[serve.static]`, `[bundle]`, `jsx*`, `[debug]`, `macros`,
`external`, `[loader]`. See `tests/test_bunfig.cpp` for per-segment vectors.

## Recorded defaults and boundaries

- Missing values preserve the destination context defaults. `serve.port = 0`
  maps to `3000`; `console.depth = 0` maps to `u16::max`.
- `logLevel` accepts only `debug`, `error`, `warn`, and `info`.
- `preload` accepts a string or array of strings; null is a no-op; empty
  strings are ignored. Pattern arrays for concurrent/path ignores require
  string items; `concurrentTestGlob` rejects empty strings and empty arrays.
- `test.seed` requires effective `randomize = true`; `retry` and `rerunEach`
  are mutually exclusive. Coverage thresholds accept one number or an object
  containing `functions`, `lines`, and/or `statements`.
- `env` accepts boolean, null, or an object whose `file` is boolean/null;
  false/null disable default env files.
- Auto-install accepts boolean or `force`/`fallback`/`disable` strings;
  `install.prefer` accepts only `online`/`offline`.

## Deferred

Source-located diagnostics (line/column), macro import-replacement remapping
(stored as a flat string map here), Pnpm matcher regex compilation for
`hoistPattern`/`publicHoistPattern` (kept as raw patterns), full WHATWG URL
normalization for registry strings (a focused userinfo/authority split is used),
filesystem config discovery, global config/autoload rules, CLI argument
precedence, and the Context/TransformOptions/BunInstall destination interfaces
remain TODO.
