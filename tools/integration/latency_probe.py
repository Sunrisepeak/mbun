#!/usr/bin/env python3
"""latency_probe — catch absurdly slow API calls that the corpus cannot see.

WHY THIS EXISTS: `dns.lookup("127.0.0.1")` took **8011 ms** in this runtime. A
literal IP needs no query at all; the resolver had one worker thread and every
lookup waited out the full query budget. It survived because nothing measured
it: the corpus runner reports pass/fail, so an 8-second call is invisible until
it crosses a 15-second timeout, and even then it is reported as "timeout", which
reads like a hang rather than like latency. After the fix the same call takes
3 ms — a 2670x difference that no compatibility number moved.

So this is deliberately NOT a benchmark. `benchmarks/` measures throughput of
things we already know are hot. This asserts an *order of magnitude* on calls
that should be trivially fast, so a 1000x regression fails a check instead of
hiding inside a timeout bucket. Thresholds sit ~10-50x above the observed good
value: catching a catastrophe, never flagging normal variance.

HONEST LIMIT, measured: this tool does **not** reproduce the dns case that
motivated it. A cold `dns.lookup("127.0.0.1")` takes 197ms on the broken binary
and 3ms on the fixed one — both far under any sane threshold. A second attempt
that queued a literal behind a non-resolving name scored 129.9ms on *both*
binaries, because the `.invalid` name fails without ever issuing a query, so
there was no slow query to block on. The real 8011ms needed that specific
deadlock: node's dgram resolves through `dns.lookup` before every `send()`, and
the test's fake nameserver was a dgram socket in the same process, so the lookup
for its reply queued behind the very resolve it would have unblocked. That case
is guarded by its corpus files (`test-dns-resolvesrv`, `test-dns-multi-channel`,
`test-dns-resolvesrv-econnrefused`), not by a probe here.

What this tool *does* buy: a cheap floor under common calls, and a baseline gate
(`--baseline`) that catches a multiple-x slowdown while the absolute number is
still small — which is where a threshold alone is blind.

Runs through bounded_run.BoundedRun, so a probe that hangs is killed and
reported rather than wedging the harness.

Usage:
    latency_probe.py --bin <mbun>                       # check against thresholds
    latency_probe.py --bin <mbun> --save perf.json      # record a baseline
    latency_probe.py --bin <mbun> --baseline perf.json  # gate on regression vs baseline
    latency_probe.py --bin <mbun> --only dns            # one probe

Exit codes: 0 all good; 1 a threshold or the baseline gate was exceeded;
2 a probe failed to run at all (which is also a bug worth failing on).
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from bounded_run import BoundedRun, ensure_disk_headroom


@dataclass(frozen=True)
class Probe:
    name: str
    threshold_ms: float
    source: str
    note: str = ""


# Each probe prints nothing; we time the whole process and subtract a measured
# startup floor, so a probe body must do ONLY the operation under test.
PROBES: tuple[Probe, ...] = (
    Probe("startup", 400, "", "process startup floor, subtracted from every other probe"),
    Probe(
        "dns.lookup-literal", 300,
        'require("dns").lookup("127.0.0.1", () => {});',
        "the regression this tool was written for: 8011ms before a grow-on-demand "
        "resolver pool, 3ms after. A literal IP must never reach a resolver.",
    ),
    Probe(
        "dns.lookup-localhost", 500,
        'require("dns").lookup("localhost", () => {});',
        "goes through the resolver but should hit the hosts file",
    ),
    Probe(
        "fs.readFileSync-x1000", 800,
        'const fs=require("fs");for(let i=0;i<1000;i++)fs.readFileSync("/etc/hostname");',
        "per-call syscall overhead; a regression here taxes every corpus file",
    ),
    Probe(
        "http-roundtrip", 800,
        'const http=require("http");const s=http.createServer((q,r)=>r.end("x"));'
        's.listen(0,()=>{http.get({port:s.address().port},(r)=>{r.resume();'
        'r.on("end",()=>s.close());});});',
        "one loopback request end to end",
    ),
    Probe(
        "net-roundtrip", 700,
        'const net=require("net");const s=net.createServer(c=>c.end("x"));'
        's.listen(0,()=>{const c=net.connect(s.address().port,()=>{});'
        'c.resume();c.on("end",()=>s.close());});',
        "raw socket accept + write + FIN",
    ),
    Probe(
        "setImmediate-chain-x10000", 400,
        'let n=0;const t=()=>{if(++n<10000)setImmediate(t);};t();',
        "a chain where each setImmediate is queued FROM an immediate, so every link "
        "needs its own check-phase turn. Real node v24 does this in 10ms; mbun did "
        "12ms until a drain-batch stamp made it 209ms — a 17x regression that changed "
        "no observable semantics (baseline, regressed build and node all print A,B,A2). "
        "Threshold set well above node but far below the regression.",
    ),
    Probe(
        "promise-chain-x1000000", 450,
        'let p=Promise.resolve(0);for(let i=0;i<1000000;i++)p=p.then(v=>v+1);'
        'p.then(v=>{if(v!==1000000)throw new Error("bad chain "+v);});',
        "1M sequential .then links -- sized so the WORK dominates process startup.\n        "
        "At 200k the work was ~15ms against ~150ms startup, so a 6x regression moved\n        "
        "the total only 1.5x and slipped under any tolerable limit.\n        "
        "Originally 200k sequential .then links. This probe exists because the async_hooks port "
        "made Promise.prototype.then instrumentation UNCONDITIONAL — one closure per "
        ".then — which took this from 16ms to 90-133ms (5.6-8x). Nothing in this gate "
        "caught it: every other probe here is dominated by process startup, so a "
        "promise-throughput regression was invisible. Threshold is well above the "
        "healthy figure and far below the regression.",
    ),
    Probe(
        "promise-chain-als-x1000000", 450,
        'const {AsyncLocalStorage}=require("async_hooks");const als=new AsyncLocalStorage();'
        'als.run("v",()=>{let p=Promise.resolve(0);'
        'for(let i=0;i<1000000;i++)p=p.then(v=>v+1);'
        'p.then(v=>{if(v!==1000000)throw new Error("bad chain "+v);});});',
        "the same chain, but with AsyncLocalStorage ADOPTED. This case exists because "
        "the plain probe cannot see it: async-context adoption is lazy, so a program "
        "that never touches ALS never installs the engine slot, and a host call landing "
        "on every .then would be invisible to the probe above. It is also the case that "
        "improved most when the engine took ownership of the frame -- once the engine "
        "owns it, `then` must NOT capture it, or it overwrites the engine's own answer "
        "and puts a host call on the hottest path. Measured 98-103ms before that fix, "
        "41-42ms after. READ IT AGAINST promise-chain-x1000000, not just against "
        "the limit: the two should be within noise of each other (measured 214.3 vs "
        "214.2). ALS costing materially more than plain means `then` started "
        "capturing the frame again, which is the regression this probe exists for.",
    ),
    Probe(
        "promise-fanout-x600000", 700,
        'const a=[];let n=0;for(let i=0;i<600000;i++)a.push(Promise.resolve(i)'
        '.then(x=>{n+=x;}));Promise.all(a).then(()=>{if(n===0)throw new Error("no work");});',
        "200k independent promises resolved and awaited together — catches per-promise "
        "allocation cost that a single chain can hide. Healthy is ~51ms; the "
        "unconditional-instrumentation regression put it at 131-188ms.",
    ),
    Probe(
        "timer-drain-x10000", 900,
        'let n=0;const t=()=>{if(++n<10000)setImmediate(t);};t();',
        "event-pump overhead per turn",
    ),
    Probe(
        "util.inspect-x5000", 900,
        'const u=require("util");const o={a:1,b:[1,2,3],c:{d:"e"}};'
        'for(let i=0;i<5000;i++)u.inspect(o);',
        "every failing assertion in the corpus formats through inspect; a previous "
        "wholesale replacement with Proxy tracking caused a catastrophic regression",
    ),
    Probe(
        "require-x2000", 900,
        'for(let i=0;i<2000;i++){delete require.cache;require("path");require("url");}',
        "module resolution on a warm cache",
    ),
)


def measure(binary: Path, probe: Probe, out_dir: Path, repeats: int, timeout: float) -> tuple[float, str]:
    """Return (best_ms, error). Best-of-N, because we are looking for orders of
    magnitude and the minimum is the least noisy estimator of a floor."""
    script = out_dir / f"probe-{probe.name.replace('.', '_').replace('/', '_')}.js"
    script.write_text(probe.source, encoding="utf-8")
    times: list[float] = []
    for attempt in range(repeats):
        log = out_dir / f"{script.stem}.{attempt}.log"
        started = time.monotonic()
        result = BoundedRun(log, private_tmp=out_dir / "tmp" / script.stem).run(
            [str(binary), str(script)], timeout=timeout)
        elapsed = (time.monotonic() - started) * 1000
        if result.timed_out:
            return (elapsed, f"timed out after {timeout}s")
        if result.exit_code != 0:
            tail = log.read_text(errors="replace").strip().splitlines()[-1:] or ["<no output>"]
            return (elapsed, f"exit {result.exit_code}: {tail[0][:80]}")
        times.append(elapsed)
    return (min(times), "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--out", type=Path, default=Path("target/integration/latency"))
    parser.add_argument("--repeats", type=int, default=3, help="best-of-N per probe")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--only", default="", help="substring filter on probe name")
    parser.add_argument("--save", type=Path, help="write results as a baseline")
    parser.add_argument("--baseline", type=Path, help="gate against a saved baseline")
    parser.add_argument("--tolerance", type=float, default=2.0,
                        help="baseline gate: fail when a probe is this many times slower")
    args = parser.parse_args()

    ensure_disk_headroom()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    binary = args.bin.resolve()

    selected = [p for p in PROBES if args.only in p.name]
    if not selected:
        raise SystemExit(f"no probe matches --only {args.only!r}")

    baseline = json.loads(args.baseline.read_text()) if args.baseline else {}
    results: dict[str, float] = {}
    floor = 0.0
    failures: list[str] = []
    broken: list[str] = []

    print(f"{'probe':26} {'ms':>8} {'limit':>8}  verdict")
    for probe in selected:
        if not probe.source:  # the startup floor probe
            (out_dir / "probe-startup.js").write_text("", encoding="utf-8")
            floor, error = measure(binary, Probe("startup", probe.threshold_ms, ""), out_dir,
                                   args.repeats, args.timeout)
            results[probe.name] = round(floor, 1)
            verdict = "ok" if not error and floor <= probe.threshold_ms else (error or "OVER")
            if error:
                broken.append(f"startup: {error}")
            elif floor > probe.threshold_ms:
                failures.append(f"startup {floor:.0f}ms > {probe.threshold_ms:.0f}ms")
            print(f"{probe.name:26} {floor:8.1f} {probe.threshold_ms:8.0f}  {verdict}")
            continue

        raw, error = measure(binary, probe, out_dir, args.repeats, args.timeout)
        # Subtract the startup floor: we are timing the operation, not the runtime's boot.
        net = max(0.0, raw - floor)
        # ...but a clamp to zero is a LIE, and it made this tool report "ok" for
        # nine probes in a row while measuring nothing. Startup varied 165ms vs
        # 115ms between two binaries, so every operation cheaper than that spread
        # subtracted to 0.0 and passed. Measured directly instead, the same pair
        # differed 12ms vs 209ms on a setImmediate chain — a 17x regression this
        # tool called "ok" twice.
        #
        # So when the subtraction bottoms out, say so rather than printing a
        # number that is not one. The floor itself is the thing to compare across
        # binaries in that case.
        floored = raw > 0 and net <= 0.05 * max(raw, 1.0)
        results[probe.name] = round(net, 1)
        if error:
            broken.append(f"{probe.name}: {error}")
            print(f"{probe.name:26} {net:8.1f} {probe.threshold_ms:8.0f}  BROKEN — {error}")
            continue
        verdict = "ok" if not floored else "UNRESOLVED (< startup noise; compare raw/floor)"
        if net > probe.threshold_ms:
            failures.append(f"{probe.name} {net:.0f}ms > {probe.threshold_ms:.0f}ms threshold")
            verdict = "OVER THRESHOLD"
        prior = baseline.get(probe.name)
        if prior is not None and prior > 1.0 and net > prior * args.tolerance:
            failures.append(f"{probe.name} {net:.0f}ms is {net / prior:.1f}x the baseline {prior:.0f}ms")
            verdict = f"REGRESSED {net / prior:.1f}x"
        print(f"{probe.name:26} {net:8.1f} {probe.threshold_ms:8.0f}  {verdict}")

    if args.save:
        args.save.parent.mkdir(parents=True, exist_ok=True)
        args.save.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"\nbaseline written to {args.save}")

    if broken:
        print("\nprobes that could not run (a bug in its own right):", file=sys.stderr)
        for line in broken:
            print(f"  {line}", file=sys.stderr)
    if failures:
        print("\nLATENCY FAILURES:", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
    if broken:
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
