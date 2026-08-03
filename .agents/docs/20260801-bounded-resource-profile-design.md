# Bounded corpus resource profiles — minimal design

## Goal

Allow an explicitly selected, spawn-heavy Linux corpus lane to use a larger
but still bounded systemd scope, while preserving the shared runner's safe
default. This makes the measured result distinguish a runtime failure from a
scope-induced `fork()`/OOM boundary.

## Evidence

- `spawn-stdin-readable-stream.test.ts` runs 50 child processes concurrently in
  its object-count test.
- The default `BoundedRun` profile is `MemoryMax=4G`, `MemorySwapMax=0`, and
  `TasksMax=512`; the lane reports `spawnEx: fork() failed` in that scope.
- The same complete upstream file with a single-lane `34G / 1024 tasks` scope
  reports **28 pass, 2 TODO, 0 fail**. No upstream test file is changed.

## Scope and invariants

1. `BoundedRun.run()` accepts optional `memory_max` and `tasks_max`; omitted
   values remain exactly the existing defaults.
2. `bun_corpus_runner.py` exposes the overrides as explicit CLI options and
   records the selected profile in its result metadata/summary.
3. An override above the default profile requires `--jobs 1`. This prevents a
   34G lane from being multiplied by the campaign's normal 3–5 lane fan-out.
4. The disk headroom gate, `MemorySwapMax=0`, timeout, private TMPDIR, and
   process-tree cleanup remain unchanged.
5. No filename-based auto-escalation, full-corpus default change, or `compat/`
   test modification is allowed.

## Interface

```text
python3 tools/integration/bun_corpus_runner.py \
  --memory-max 34G --tasks-max 1024 --jobs 1 \
  --list <root-relative-list> --out <result-dir> ...
```

The summary includes a sanitized profile such as `memory_max=34G`
and `tasks_max=1024`; it never includes local absolute paths or environment
values beyond the existing root-relative test identities.

## Test plan

- Extend the bounded runner self-test with a harmless command that captures the
  generated scope arguments, proving defaults, explicit overrides, and the
  `jobs > 1` rejection without spawning a real corpus workload.
- Run the Bun corpus runner's focused stream file with `--jobs 1`, `34G`, and
  `1024`; expect **28 pass, 2 TODO, 0 fail**.
- Run the existing four-file default-profile probe again only if needed for a
  regression comparison; do not raise its defaults or start a full corpus run.

## Risk and rollback

The only behavior change is opt-in argument plumbing. If the self-test or
focused lane shows resource leakage, remove the CLI override and retain the
existing default profile; no runtime source rollback is required.
