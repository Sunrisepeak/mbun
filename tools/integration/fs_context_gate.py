#!/usr/bin/env python3
"""fs_context_gate -- pin async-context propagation across node:fs callbacks.

Why this exists as a gate rather than a corpus test:

The async context active when an fs call is made must still be active when its
callback runs. That is what makes AsyncLocalStorage survive an fs round trip
and what lets a domain catch a throw out of an fs callback. In node it falls
out of the architecture (the callback is an FSReqCallback owned by AsyncWrap);
in mbun it has to be done by hand at every place that defers a callback.

It was broken exactly that way in w65/asyncfs: a batched fs completion drain
was routed through the raw host immediate -- correct for keeping the drain out
of the Immediate accounting, and at the same time an opt-out of the context
propagation that node:timers does for setTimeout/setImmediate. Every callback
through it lost its AsyncLocalStorage store, and a domain could no longer catch
a throw out of one.

That is the signature of a contract worth pinning explicitly: silent when
broken (a lost store reads as `undefined`, not as an error), and close to
invisible to a file-count gate -- ONE corpus file in 1955 changed bucket. Two
full corpus rounds over 2183 files showed +2/-0 and said nothing about it.

The invariants below are node's documented behaviour, not measurements of
mbun: an fs callback runs in the async resource of its call site, full stop.
Add an entry point here whenever one is given a new deferral path.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# The probe prints one `name=value` line per entry point, so the output is
# order-independent and diffable. Every value must be `ok`.
FIXTURE = r"""
const fs = require('fs'), os = require('os'), path = require('path');
const { AsyncLocalStorage } = require('async_hooks');
const domain = require('domain');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mbun-fsctx-'));
const file = path.join(dir, 'f.txt');
fs.writeFileSync(file, 'hello');
const fd = fs.openSync(file, 'r');
const missing = path.join(dir, 'no-such-file');
const als = new AsyncLocalStorage();
// Printed as produced, NOT collected and sorted at the end: a domain failure is
// fatal (the throw escapes and kills the process), so anything buffered is lost
// exactly when the diagnostics matter most. The gate parses by name, so order
// carries no meaning.
const show = (n, v) => console.log(n + "=" + v);

// The store is an object identity, not a primitive: a wrapper that rebuilt the
// frame instead of restoring it would still pass a primitive comparison.
const TOKEN = { id: 42 };

const alsCase = (name, fn) => new Promise((done) => {
  let settled = false;
  const finish = (v) => { if (!settled) { settled = true; show('als_' + name, v); done(); } };
  als.run(TOKEN, () => {
    try {
      fn(() => finish(als.getStore() === TOKEN ? 'ok' : 'LOST'));
    } catch (e) { finish('THREW:' + (e && e.code || e && e.message)); }
  });
  setTimeout(() => finish('NEVER_CALLED'), 2000);
});

// A throw out of an fs callback must reach the domain that was active when the
// fs call was made. Shape of test-domain-implicit-binding.js.
const domainCase = (name, fn) => new Promise((done) => {
  const d = domain.create();
  let caught = null;
  d.on('error', (e) => { caught = e && e.message; });
  d.run(() => {
    setTimeout(() => { fn(() => { throw new Error('boom-' + name); }); }, 0);
  });
  setTimeout(() => {
    show('domain_' + name, caught === 'boom-' + name ? 'ok' : 'ESCAPED:' + caught);
    done();
  }, 120);
});

const alsCases = [
  ['stat',       (k) => fs.stat(file, k)],
  ['lstat',      (k) => fs.lstat(file, k)],
  ['fstat',      (k) => fs.fstat(fd, k)],
  ['open',       (k) => fs.open(file, 'r', (e, f) => { if (f !== undefined) fs.closeSync(f); k(); })],
  ['close',      (k) => { const t = fs.openSync(file, 'r'); fs.close(t, k); }],
  ['read',       (k) => fs.read(fd, Buffer.alloc(2), 0, 2, 0, k)],
  ['readdir',    (k) => fs.readdir(dir, k)],
  ['mkdir',      (k) => fs.mkdir(path.join(dir, 'd' + Math.random()), k)],
  ['unlink',     (k) => { const a = path.join(dir, 'u' + Math.random()); fs.writeFileSync(a, 'x'); fs.unlink(a, k); }],
  ['chmod',      (k) => fs.chmod(file, 0o644, k)],
  ['readlink',   (k) => fs.readlink(missing, () => k())],
  ['access',     (k) => fs.access(file, k)],
  ['readFile',   (k) => fs.readFile(file, k)],
  ['writeFile',  (k) => fs.writeFile(path.join(dir, 'w.txt'), 'x', k)],
  ['appendFile', (k) => fs.appendFile(path.join(dir, 'a.txt'), 'x', k)],
];

(async () => {
  for (const [n, f] of alsCases) await alsCase(n, f);
  for (const [n, f] of [['stat', (k) => fs.stat(missing, k)],
                        ['readFile', (k) => fs.readFile(missing, k)]]) {
    await domainCase(n, f);
  }
})();
"""

# Every probed name must report exactly this.
EXPECTED_VALUE = "ok"

# Named so a MISSING line is reported as such rather than silently passing --
# a probe that dies halfway through must fail the gate, not shrink it.
EXPECTED_KEYS = [
    "als_access", "als_appendFile", "als_chmod", "als_close", "als_fstat",
    "als_lstat", "als_mkdir", "als_open", "als_read", "als_readFile",
    "als_readdir", "als_readlink", "als_stat", "als_unlink", "als_writeFile",
    "domain_readFile", "domain_stat",
]


def parse(stdout: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in stdout.splitlines():
        line = line.strip()
        if "=" in line:
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return values


def check(values: dict[str, str]) -> list[str]:
    problems = []
    for key in EXPECTED_KEYS:
        got = values.get(key)
        if got is None:
            problems.append(f"{key}: MISSING (probe did not report it)")
        elif got != EXPECTED_VALUE:
            problems.append(f"{key}: got {got!r}, expected {EXPECTED_VALUE!r}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", help="runtime to test (mbun, or a real node to re-pin)")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--emit-fixture", action="store_true",
                    help="print the probe JS and exit, for checking against real node")
    args = ap.parse_args()

    if args.emit_fixture:
        print(FIXTURE)
        return 0
    if not args.bin:
        ap.error("--bin is required unless --emit-fixture is given")

    binary = Path(args.bin)
    if not binary.exists():
        print(f"fs_context_gate: no such binary: {binary}", file=sys.stderr)
        return 2

    # The probe is bounded: a runtime that hangs here is itself the regression.
    try:
        proc = subprocess.run([str(binary), "-e", FIXTURE], capture_output=True,
                              text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"fs_context_gate: FAIL -- {binary} did not finish in {args.timeout}s",
              file=sys.stderr)
        return 1

    values = parse(proc.stdout)
    problems = check(values)
    if problems:
        print("fs_context_gate: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        if proc.returncode != 0:
            # A domain failure is FATAL: the throw escapes and kills the process
            # before the probe can report, so stderr is the only evidence left.
            print(f"  (exit {proc.returncode}) {proc.stderr.strip()[:600]}", file=sys.stderr)
        return 1

    print(f"fs_context_gate: ok -- async context survives {len(EXPECTED_KEYS)} "
          f"node:fs callback paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
