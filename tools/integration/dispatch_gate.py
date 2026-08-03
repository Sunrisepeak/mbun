#!/usr/bin/env python3
"""dispatch_gate -- how many lanes may be dispatched RIGHT NOW, from measured state.

    python3 tools/integration/dispatch_gate.py              # report + exit code
    python3 tools/integration/dispatch_gate.py --want 5     # refuse (exit 1) if 5 is unsafe

WHY THIS EXISTS. The campaign's standing constraint is "at most 5 parallel lanes,
and do NOT freeze the machine or exhaust memory". That was being satisfied by the
integrator eyeballing `uptime`, `free` and `df` before each wave -- which worked
only because the integrator remembered to look. It did not always: the box hit
100% disk twice mid-wave (9.1 GB then 9.4 GB free on 1.5 TB), and both times the
first symptom was a corpus runner failing in a way that reads like a runner bug
rather than like a full disk. A wave dispatched into that state loses every lane's
time, not one lane's.

So the check is a tool with a self-test, and it answers the question the
integrator actually has -- "how many lanes can I start" -- rather than printing
three numbers to be interpreted by hand.

WHAT EACH LIMIT IS FOR, measured on this box rather than guessed:

  disk    Each lane worktree accumulates its own `target/` plus per-member
          `modules/*/target/`. Measured: a single lane worktree reached 23 GB,
          and five stale ones held 49 GB between them. A cold rebuild costs ~95 s,
          so reclaiming is cheap and running out is not -- `bounded_run` refuses
          to start a measurement without headroom, which aborts a wave mid-flight.
          Budgeted at DISK_PER_LANE_GB per lane plus a floor that must survive.

  memory  A corpus runner at --jobs 4 plus a compile is the peak. safe-test.sh
          caps a single scope at 6 G (34 G for build-shaped commands), so the
          exposure is roughly one build per lane.

  load    Load above the core count means lanes are already contending, and
          contention does not just slow things down -- it FABRICATES results. The
          same binary has been measured passing a file idle and failing it under
          load (60 files in one wave). A wave dispatched into a loaded box
          produces measurements nobody can trust.

Thresholds are overridable so the tool can be tested and so a human can widen
them deliberately; MBUN_GATE_FAKE_* exist purely for the self-test, because a
guard that depends on live machine state cannot otherwise be tested at all.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys

CEILING = 5             # the campaign's standing parallelism cap
DISK_PER_LANE_GB = 8.0  # a lane's own target/ trees, from measurement
DISK_FLOOR_GB = 15.0    # must survive the wave; below this bounded_run refuses
MEM_PER_LANE_GB = 6.0   # one safe-test scope at the default cap
LOAD_HEADROOM = 0.75    # of cores; above this, measurements stop being trustworthy


def _free_disk_gb(path: str = ".") -> float:
    v = os.environ.get("MBUN_GATE_FAKE_DISK_GB")
    if v is not None:
        return float(v)
    return shutil.disk_usage(path).free / (1024 ** 3)


def _free_mem_gb() -> float:
    v = os.environ.get("MBUN_GATE_FAKE_MEM_GB")
    if v is not None:
        return float(v)
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024 ** 2)
    except OSError:
        pass
    return float("inf")  # unknown: do not block on it


def _load1() -> float:
    v = os.environ.get("MBUN_GATE_FAKE_LOAD")
    if v is not None:
        return float(v)
    try:
        return os.getloadavg()[0]
    except OSError:
        return 0.0


def _cores() -> int:
    v = os.environ.get("MBUN_GATE_FAKE_CORES")
    if v is not None:
        return int(v)
    return os.cpu_count() or 1


def capacity() -> tuple[int, list[str]]:
    """(lanes that may start now, one reason line per limit)."""
    disk, mem, load, cores = _free_disk_gb(), _free_mem_gb(), _load1(), _cores()

    by_disk = int(max(0.0, disk - DISK_FLOOR_GB) // DISK_PER_LANE_GB)
    by_mem = int(mem // MEM_PER_LANE_GB) if mem != float("inf") else CEILING
    # Load is spare capacity, not a per-lane budget: a lane is not one core.
    spare = cores * LOAD_HEADROOM - load
    by_load = CEILING if spare >= cores * 0.25 else max(0, int(spare // 2))

    allowed = max(0, min(CEILING, by_disk, by_mem, by_load))
    reasons = [
        f"disk  {disk:6.1f} GB free  -> {by_disk} lane(s)"
        f"  (floor {DISK_FLOOR_GB:.0f} GB + {DISK_PER_LANE_GB:.0f} GB/lane)",
        f"mem   {mem:6.1f} GB avail -> {by_mem} lane(s)  ({MEM_PER_LANE_GB:.0f} GB/lane)",
        f"load  {load:6.2f} on {cores} cores -> {by_load} lane(s)"
        f"  (headroom {LOAD_HEADROOM:.0%})",
        f"cap   campaign ceiling    -> {CEILING} lane(s)",
    ]
    return allowed, reasons


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--want", type=int, help="refuse with exit 1 if this many is unsafe")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    allowed, reasons = capacity()
    if not args.quiet:
        print("=== dispatch gate ===")
        for r in reasons:
            print("  " + r)
        print(f"\n  => {allowed} lane(s) may start now")

    if args.want is not None and args.want > allowed:
        print(f"\ndispatch_gate: REFUSED -- asked for {args.want}, only {allowed} is safe.",
              file=sys.stderr)
        if allowed == 0:
            print("  Nothing may start. Run tools/integration/reclaim_disk.sh --apply "
                  "--prune-configs, or wait for the running wave to finish.", file=sys.stderr)
        else:
            print(f"  Dispatch {allowed}, or reclaim first. Starting {args.want} risks losing "
                  f"the whole wave's time, not one lane's.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
