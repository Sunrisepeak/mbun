# tools/integration — bounded execution for tests, benches, and debugging

Everything here shares one rule: **workloads that execute tests, benchmarks,
or debug targets never run bare.** They go through the shared safety layer so
a hostile workload degrades into a killed scope and a clear log — never a
frozen machine or a silently murdered harness.

## The safety layer

- `bounded_run.py` — Python module used by every runner. One
  `BoundedRun(log).run(cmd, timeout)` gives you all of:
  1. systemd `--user` scope: `MemoryMax=4G`, `MemorySwapMax=0`, `TasksMax=512`,
     `RuntimeMaxSec` — a fork-deadlock OOM-kills inside the scope instead of
     swap-thrashing the box;
  2. `TimeoutStopSec=3` — SIGKILL follows RuntimeMaxSec's SIGTERM, so children
     that ignore SIGTERM (node `child_process` suites) still die;
  3. `start_new_session` — tests that signal their whole process group
     (`kill(0, SIGABRT)`) cannot kill the harness (this silently killed three
     corpus runs before it was isolated);
  4. stdout straight to the log file, never a pipe — orphaned grandchildren
     holding a pipe stall `subprocess.run()` past its timeout and wedge the
     worker pool;
  5. private per-test `TMPDIR`, deleted afterwards — killed tests leak tmpdir
     debris (one corpus round left ~500k entries in `/tmp` and filled the
     disk);
  6. `ensure_disk_headroom()` — refuse to start when the disk is nearly full.
- `safe-test.sh` — the same protections for ad-hoc shell use:
  `tools/integration/safe-test.sh <timeout_sec> <cmd...>`. ALWAYS use it for
  one-off runs of spawn-heavy tests; `benchmarks/tools/*.sh` route every
  runner invocation through it.

## Runners

- `bun_corpus_runner.py` — bun's native test corpus (`compat/bun/test`)
  through `mbun test`, with per-file classification (green / test-failure /
  timeout / oom-kill / …). See `compat/README.md` for the measurement recipe.
- `node_corpus_runner.py` — node's `compat/node/test/parallel` executed
  directly (`mbun <file>`, exit 0 = pass); node-harness-dependent files count
  as failures, keeping the number honest file-level coverage.
- `test_members.sh` — `mcpp test` across every workspace member.

## Self-tests

Every tool has a self-test under `tests/`; run them after touching a runner:

```bash
bash tools/integration/tests/test_bun_corpus_runner.sh
bash tools/integration/tests/test_node_corpus_runner.sh
bash benchmarks/tools/test-bench3.sh
```

## Adding a new tool

Reuse, don't re-derive: import `bounded_run` (Python) or call `safe-test.sh`
(shell). If a new failure mode gets past the layer, fix it **in the layer** and
note the incident in the docstring, so every tool inherits the protection.
