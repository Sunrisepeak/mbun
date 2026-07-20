# Compatibility corpora

`compat/` is the single home for upstream compatibility inputs, runners, and
their recorded measurements.

## Layout

- `bun/` is the `oven-sh/bun` submodule, currently pinned to a `main` snapshot.
- `bun/test/` contains Bun's native test corpus.
- `compat/bun/bench/` contains Bun's native benchmark corpus.
- `node/` is the `nodejs/node` submodule, pinned to the selected Node.js release.
- `node/test/` contains Node's native test corpus.
- `data/` contains corpus inventories and benchmark result data.

Initialize the upstream repositories with:

```bash
git submodule update --init --recursive
```

The submodules are read-only inputs. mbun-specific adapters, fixtures, and
harnesses belong outside them.

## Bun corpus runner

**Prerequisite: the corpus needs its npm dependencies installed**, in *two*
places — `compat/bun` (the repo's own devDependencies) and `compat/bun/test`
(the corpus fixtures' packages):

```bash
(cd compat/bun && bun install --frozen-lockfile)
(cd compat/bun/test && bun install --frozen-lockfile)
```

Without them the numbers are badly wrong, not just slightly: **264 of the 1902
files** fail at file evaluation with `Cannot find module`, before a single
assertion runs — `esbuild` alone kills 76 files, because
`test/bundler/expectBundled.ts` imports it unconditionally at the top of the
shared bundler harness. Those files then look like one-assertion near-misses
when they are in fact dead. `node_modules/` is gitignored inside the submodule,
so a fresh clone always starts in this state. The runner refuses to start when
it sees it (`--allow-missing-node-modules` overrides, for a deliberate
measurement of the un-provisioned state).

```bash
MBUN="$(find target -type f -name mbun -perm -111 | head -n 1)"
python3 tools/integration/bun_corpus_runner.py \
  --bin "$MBUN" --cwd compat/bun \
  --discover compat/bun/test --sample-per-group 100000 \
  --out target/integration/bun-corpus --jobs 14 --timeout 30
```

`--cwd compat/bun` matters: bun's own CI runs from the bun repo root, so tests
that spawn relative fixture paths and the corpus `bunfig.toml` only work there.
`summary.json` reports per-file classifications; `green` counts files whose
every executed test passed and which reported no error outside a test.
Buckets that are explicitly **not** passes: `all-skipped` (every test skipped),
`no-tests` (the upstream file declares no runnable test), and
`blocked-external` (needs a service, registry, or toolchain this environment
does not have — the hand-triaged list is
`tools/integration/manifests/blocked-external.txt`).

## Node corpus runner

Node's upstream `test/parallel` files are plain scripts that expect exit
code 0 (no test runner). The runner executes each file directly through mbun;
harness services Node's own runner would provide (`// Flags:` comments,
internal bindings, env setup) are NOT emulated, so files that need them count
as failures. The result is honest file-level coverage, not an API checklist.

```bash
python3 tools/integration/node_corpus_runner.py \
  --bin "$MBUN" \
  --out target/integration/node-parallel --jobs 14 --timeout 15
```

Measurement data is stored under [`compat/data/`](data/), including test and
benchmark inventories and native run results.
