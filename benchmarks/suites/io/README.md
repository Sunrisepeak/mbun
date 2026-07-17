# mbun.core.io benchmark suite

This suite derives from bun's `compat/bun/bench/snippets/read-file.mjs` and
`compat/bun/bench/snippets/write-file.mjs`, narrowed to the layer implemented by
`mbun.core.io`: cached positioned reads and writes on already-open descriptors
at 4 KiB and 1 MiB. Open/close, allocation, JS string conversion, and truncate
costs are outside the timed region. Every case calibrates independently to a
minimum 250 ms sample, runs at least ten rounds in shuffled order, and emits
the raw duration, iteration count, and checksum for every round. The C++ binary
measures three caller-buffer layouts (ordinary `std::vector`, 4 KiB alignment,
and a 2 MiB-aligned `MADV_HUGEPAGE` candidate) plus libc `pread`/`pwrite` on
separate descriptors in the same process. This separates module-wrapper
overhead from kernel/filesystem and caller-memory-layout costs.

```bash
.mbun/bin/bun-zig benchmarks/suites/io/bench.mjs 250 10
.mbun/bin/bun-rust benchmarks/suites/io/bench.mjs 250 10
cd modules/core
mcpp run bench-core-io -- 250 10
```

For lower scheduling noise on Linux, pin all three commands to the same idle
core, for example `taskset -c 2 <command>`. Do not mix pinned and unpinned runs.
The reported comparison is the median ops/s derived from the ten raw rounds;
retain every round in the report rather than selecting the best result. The bun
driver retains the unavoidable `node:fs` binding cost around the same
pread/pwrite operations; the mbun driver measures the engine-independent
`mbun.core.io` layer. Whole-file public API, copy-file, and async stream
benchmarks remain with T3.5 and later runtime layers.

Buffer alignment is part of the result, not an assumed constant. The C++ JSON
records address moduli for all layouts. Bun's driver uses the runtime's normal
`Buffer.allocUnsafe`; its actual alignment can be inspected with `ptr(buffer)`
from `bun:ffi`. A THP-aligned C++ result represents an optional native caller
layout and must be reported separately from the ordinary/page-aligned comparison.
