#!/usr/bin/env python3
"""tick_order_gate -- pin node's process.nextTick / microtask ordering contract.

Why this exists as a gate rather than a corpus test:

mbun shipped for a long time with `process.nextTick` armed by a promise
reaction, which inverted node's core rule that the microtask queue drains
FULLY before the tick queue runs. A tick scheduled from inside a microtask
interleaved into the middle of that microtask chain. No corpus file names
this contract, so nothing caught it directly -- it surfaced only as a hanging
`compose` test, and it turned out that THREE subsystems had grown to depend
on the broken interleaving (the http client's connect re-defer, fs.promises
writeFile/appendFile abort handling, and the test runner's `done()` probe),
each of which was masking an ordering bug of its own.

That is the signature of a contract worth pinning explicitly: broken for a
long time, depended upon once broken, and invisible to every existing gate.

The expected values below were measured against a real node (24.4.1), not
written from memory. Regenerate them the same way if node's behaviour moves:

    node tools/integration/tick_order_gate.py --emit-fixture > /tmp/probe.js
    node /tmp/probe.js
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# The probe prints one `name=value` line per invariant, sorted, so the output is
# order-independent and diffable.
FIXTURE = r"""
const out = [];
const show = (n, v) => out.push(n + "=" + v);

// 1. A microtask chain drains FULLY before a tick queued from inside it.
//    This is the invariant that was inverted: mbun printed mt1,TICK,mt2.
{
  const o = [];
  Promise.resolve().then(() => { o.push("mt1"); process.nextTick(() => o.push("TICK")); })
                   .then(() => o.push("mt2"));
  setTimeout(() => show("microtasks_before_tick", o.join(",")), 5);
}
// 2. Ticks run before both timers and immediates.
{
  const o = [];
  setTimeout(() => o.push("timeout"), 0);
  setImmediate(() => o.push("immediate"));
  process.nextTick(() => o.push("tick"));
  setTimeout(() => show("tick_before_timer_and_immediate", o.join(",")), 5);
}
// 3. The tick queue is FIFO.
{
  const o = [];
  process.nextTick(() => o.push(1));
  process.nextTick(() => o.push(2));
  process.nextTick(() => o.push(3));
  setTimeout(() => show("tick_fifo", o.join(",")), 5);
}
// 4. A tick queued from inside a tick still beats timers.
{
  const o = [];
  setTimeout(() => o.push("timeout"), 0);
  process.nextTick(() => { o.push("t1"); process.nextTick(() => o.push("t2")); });
  setTimeout(() => show("nested_tick_before_timer", o.join(",")), 5);
}
// 5. A microtask queued inside a tick runs after the WHOLE tick queue drains,
//    not immediately after the tick that queued it.
{
  const o = [];
  process.nextTick(() => { o.push("tickA"); Promise.resolve().then(() => o.push("mt")); });
  process.nextTick(() => o.push("tickB"));
  setTimeout(() => show("microtask_inside_tick", o.join(",")), 5);
}
// 6. A rejection whose .catch() is attached from a tick must NOT be reported
//    unhandled. node runs processPromiseRejections AFTER the tick queue; a
//    full VM::drainMicrotasks() here reports it and in mbun a false
//    unhandledRejection is fatal.
{
  let unhandled = 0;
  process.on("unhandledRejection", () => { unhandled++; });
  const p = Promise.reject(new Error("x"));
  process.nextTick(() => p.catch(() => {}));
  setTimeout(() => show("catch_from_tick_unhandled_count", unhandled), 20);
}
// 7. A long microtask chain that each arm a tick must not exhaust the stack.
//    Clearing the armed flag before draining nests one native drain per chain
//    link; this catches that regression.
{
  let n = 0;
  let chain = Promise.resolve();
  for (let i = 0; i < 3000; i++) chain = chain.then(() => { process.nextTick(() => { n++; }); });
  chain.then(() => setTimeout(() => show("deep_chain_ticks_ran", n), 5));
}
setTimeout(() => { out.sort(); console.log(out.join("\n")); }, 120);
"""

# Measured on node 24.4.1. Every entry is an exact match EXCEPT the one noted.
EXPECTED = {
    "microtasks_before_tick": "mt1,mt2,TICK",
    "tick_fifo": "1,2,3",
    "nested_tick_before_timer": "t1,t2,timeout",
    "microtask_inside_tick": "tickA,tickB,mt",
    "catch_from_tick_unhandled_count": "0",
    "deep_chain_ticks_ran": "3000",
}

# setImmediate vs setTimeout(0) is explicitly NON-DETERMINISTIC in node when
# called from the main module -- 8 runs of node 24.4.1 produced
# "tick,immediate,timeout" 7 times and "tick,timeout,immediate" once. Only the
# part that IS a contract gets pinned: the tick comes first. Pinning the whole
# string would have baked in a flake and, worse, reported a false divergence
# against mbun, which matches node's common case.
PREFIX_EXPECTED = {"tick_before_timer_and_immediate": "tick,"}


def parse(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            values[key.strip()] = value.strip()
    return values


def check(values: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for key, want in sorted(EXPECTED.items()):
        got = values.get(key)
        if got is None:
            problems.append(f"{key}: MISSING (probe did not report it)")
        elif got != want:
            problems.append(f"{key}: got {got!r}, expected {want!r}")
    for key, want_prefix in sorted(PREFIX_EXPECTED.items()):
        got = values.get(key)
        if got is None:
            problems.append(f"{key}: MISSING (probe did not report it)")
        elif not got.startswith(want_prefix):
            problems.append(f"{key}: got {got!r}, expected it to start with {want_prefix!r}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", help="runtime to test (mbun, or a real node to re-pin)")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--emit-fixture", action="store_true",
                    help="print the probe JS and exit, for re-pinning against real node")
    args = ap.parse_args()

    if args.emit_fixture:
        print(FIXTURE)
        return 0
    if not args.bin:
        ap.error("--bin is required unless --emit-fixture is given")

    binary = Path(args.bin)
    if not binary.exists():
        print(f"tick_order_gate: no such binary: {binary}", file=sys.stderr)
        return 2

    # The probe is bounded: a runtime that hangs here is itself the regression.
    try:
        proc = subprocess.run([str(binary), "-e", FIXTURE], capture_output=True,
                              text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"tick_order_gate: FAIL -- {binary} did not finish in {args.timeout}s",
              file=sys.stderr)
        return 1

    values = parse(proc.stdout)
    problems = check(values)
    if problems:
        print("tick_order_gate: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        if proc.returncode != 0:
            print(f"  (exit {proc.returncode}) {proc.stderr.strip()[:400]}", file=sys.stderr)
        return 1

    print(f"tick_order_gate: ok -- {len(EXPECTED) + len(PREFIX_EXPECTED)} "
          f"ordering invariants hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
