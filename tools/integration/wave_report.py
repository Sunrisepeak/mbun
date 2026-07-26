#!/usr/bin/env python3
"""wave_report — measure a wave, project the finish, and say what to change.

WHY THIS EXISTS: "evaluate and optimise the strategy after each wave" is only
worth anything if the evaluation is the same every time and cannot be talked
into a flattering answer. Doing it by hand has already produced two wrong
strategic calls in this project: an architectural push was proposed as the main
constraint when measurement showed it covers 16% of the remaining work, and a
"100% in 100 minutes" target was passed through to three agents when the
arithmetic ruled it out before dispatch.

So this computes four things and refuses to editorialise:

  1. What the wave actually moved (gains, regressions, net) — per file, from the
     two runs, never from what an agent reported.
  2. Throughput in files per wall-clock hour, which is the only number that
     projects.
  3. Time to 100% at that rate, stated plainly even when the answer is
     unwelcome.
  4. Where the remaining work SITS — mechanism-shaped clusters versus long tail
     — because that is what decides which dispatch protocol is correct, and it
     is the thing most easily assumed rather than measured.

Usage:
    wave_report.py --before <run-dir> --after <run-dir> --corpus node \\
        --hours 2.0 --agents 3 [--json report.json]

Exit codes: 0 always. This informs a decision; it does not gate one.
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import sys
from pathlib import Path

GREEN = {"pass", "green", "ahead-of-reference"}
NEUTRAL = {"skipped", "all-skipped", "no-tests"}


def load(run: Path) -> dict[str, str]:
    rows = {}
    text = (run / "results.tsv").read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    header = lines[0].split("\t")
    cls_i = header.index("classification")
    for line in lines[1:]:
        f = line.split("\t")
        if len(f) > cls_i:
            rows[f[0].rsplit("/", 1)[-1]] = f[cls_i]
    return rows


def shape_of_remaining(run: Path, corpus: str, root: Path) -> dict:
    """Cluster the still-failing files by signature, and split mechanism-shaped
    work from long tail. This is the number that decides the protocol."""
    spec = importlib.util.spec_from_file_location(
        "failure_signature", root / "tools" / "integration" / "failure_signature.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["failure_signature"] = mod
    spec.loader.exec_module(mod)
    sign = mod.bun_signature if corpus == "bun" else mod.node_signature

    groups: dict[str, list[str]] = collections.defaultdict(list)
    confidence: dict[str, str] = {}
    for path, classification, log in mod.load_rows(run, corpus):
        log_path = run / log
        if not log_path.exists():
            continue
        sig = (mod.timeout_signature() if classification in ("timeout", "oom-kill")
               else sign(log_path.read_text(encoding="utf-8", errors="replace")))
        groups[sig.key].append(path)
        confidence[sig.key] = sig.confidence

    total = sum(len(v) for v in groups.values())
    # "Mechanism-shaped" = a NAMED cause with enough files that finding it pays
    # for itself. Everything else is long tail, whatever its bucket size: a
    # 90-file bucket of bare assertions is 90 separate investigations wearing one
    # label, which is the trap that cost this project three rounds.
    mech = sum(len(v) for k, v in groups.items()
               if confidence[k] in ("CAUSE", "CLASS") and len(v) >= 5)
    tail = sum(len(v) for v in groups.values() if len(v) <= 4)
    unnamed = sum(len(v) for k, v in groups.items()
                  if confidence[k] in ("MANIFESTATION", "UNSPLIT"))
    return {"files": total, "signatures": len(groups),
            "files_per_signature": round(total / max(1, len(groups)), 1),
            "mechanism_shaped": mech, "long_tail_le4": tail, "unnamed": unnamed}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--before", required=True, type=Path)
    ap.add_argument("--after", required=True, type=Path)
    ap.add_argument("--corpus", required=True, choices=("bun", "node"))
    ap.add_argument("--hours", type=float, required=True, help="wall-clock hours the wave took")
    ap.add_argument("--agents", type=int, required=True)
    ap.add_argument("--root", type=Path, default=Path.cwd())
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    before, after = load(args.before), load(args.after)
    common = set(before) & set(after)
    gains = sorted(f for f in common if before[f] not in GREEN and after[f] in GREEN)
    regressions = sorted(f for f in common if before[f] in GREEN and after[f] not in GREEN)
    net = len(gains) - len(regressions)

    remaining = sum(1 for f, c in after.items() if c not in GREEN and c not in NEUTRAL)
    rate = net / args.hours if args.hours else 0.0
    per_agent = net / args.agents if args.agents else 0.0

    print(f"wave: +{len(gains)} gained, -{len(regressions)} regressed, net {net:+d}")
    if regressions:
        print("  regressions (reproduce each standalone before believing them):")
        for f in regressions[:10]:
            print(f"    {f}: {before[f]} -> {after[f]}")
    print(f"throughput: {rate:.0f} files/hour over {args.hours:.1f}h "
          f"with {args.agents} agents ({per_agent:.1f} files/agent)")
    print(f"remaining: {remaining}")
    if rate > 0:
        print(f"time to 100% at this rate: {remaining / rate:.0f} hours")
    else:
        print("time to 100% at this rate: never — this wave moved nothing")

    shape = shape_of_remaining(args.after, args.corpus, args.root.resolve())
    print(f"\nshape of what is left: {shape['files']} files / {shape['signatures']} signatures "
          f"= {shape['files_per_signature']} files each")
    print(f"  mechanism-shaped (named cause, >=5 files): {shape['mechanism_shaped']}")
    print(f"  long tail (clusters of <=4):               {shape['long_tail_le4']}")
    print(f"  no named cause at all:                     {shape['unnamed']}")

    # The protocol recommendation follows from the shape, not from taste.
    mech_share = shape["mechanism_shaped"] / max(1, shape["files"])
    print("\nwhat this implies for the next wave:")
    if mech_share >= 0.4:
        print("  MECHANISM protocol — enough files sit behind shared named causes that")
        print("  finding one pays for itself. Brief per cause, few agents, wide guards.")
    else:
        print(f"  LONG-TAIL protocol — only {mech_share:.0%} of what is left sits behind a")
        print("  shared named cause. Briefing per mechanism costs more than the repair.")
        print("  Dispatch short disjoint worklists (make_worklists.py), many agents,")
        print("  narrow per-agent guards, full corpus at integration only.")
    if shape["unnamed"] > shape["files"] * 0.25:
        print(f"  ALSO: {shape['unnamed']} files ({shape['unnamed']/shape['files']:.0%}) have no named")
        print("  cause. That is un-dispatchable work, and improving the extractor or")
        print("  splitting those buckets buys more than another repair wave.")

    if args.json:
        args.json.write_text(json.dumps(
            {"gains": gains, "regressions": regressions, "net": net, "hours": args.hours,
             "agents": args.agents, "files_per_hour": round(rate, 1), "remaining": remaining,
             "hours_to_100pct": round(remaining / rate, 1) if rate > 0 else None,
             "shape": shape}, indent=1) + "\n", encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
