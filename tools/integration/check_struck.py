#!/usr/bin/env python3
"""check_struck -- has this work already been tried and retired?

Run this BEFORE starting a lane, with whatever words describe your target:

    python3 tools/integration/check_struck.py bunfig preload
    python3 tools/integration/check_struck.py --area bun bundler splitting
    python3 tools/integration/check_struck.py --list          # everything

Exit 0 = nothing on record, go ahead.
Exit 3 = a matching entry exists. That is not a veto -- it is the measurement
         somebody already paid for. Read it, and if you still think the vein is
         live, say in your report which entry you are overturning and why.

The registry lives in struck.tsv. It exists because the `bunfig` preload change
was implemented and reverted TWICE, costing the same 5 bun files each time: the
first retirement was recorded only as prose in RESUME.md, which no lane reads end
to end.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REGISTRY = Path(__file__).resolve().parent / "struck.tsv"

# Rows whose verdict means "this actively loses files" are worth shouting about;
# the rest are informational.
LOUD = {"COSTS_GREEN", "DECLINED"}


def load(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5 or parts[0] == "area":
            continue
        rows.append({
            "area": parts[0].strip(),
            "target": parts[1].strip(),
            "verdict": parts[2].strip(),
            "cost": parts[3].strip(),
            "evidence": parts[4].strip(),
        })
    return rows


def matches(row: dict[str, str], terms: list[str], area: str | None) -> bool:
    if area and row["area"].lower() != area.lower():
        return False
    if not terms:
        return True
    haystack = f"{row['area']} {row['target']} {row['evidence']}".lower()
    # Every term must appear somewhere in the row -- an AND, so a two-word query
    # narrows instead of flooding.
    return all(term.lower() in haystack for term in terms)


def render(row: dict[str, str]) -> str:
    mark = "!!" if row["verdict"] in LOUD else "  "
    return (f"{mark} [{row['verdict']}] {row['area']}: {row['target']}\n"
            f"     cost/size: {row['cost']}\n"
            f"     evidence:  {row['evidence']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("terms", nargs="*", help="words describing your target (ANDed)")
    ap.add_argument("--area", help="restrict to one area (node, bun, jsc)")
    ap.add_argument("--list", action="store_true", help="print the whole registry")
    ap.add_argument("--registry", type=Path, default=REGISTRY)
    args = ap.parse_args()

    rows = load(args.registry)
    if not rows:
        print(f"check_struck: registry is empty or missing: {args.registry}",
              file=sys.stderr)
        return 2

    if args.list:
        for row in rows:
            print(render(row))
        print(f"\n{len(rows)} entries in {args.registry}")
        return 0

    if not args.terms and not args.area:
        ap.error("give some terms describing your target, or --list")

    hits = [row for row in rows if matches(row, args.terms, args.area)]
    if not hits:
        print("check_struck: nothing on record for that target -- go ahead.")
        return 0

    query = " ".join(args.terms) or f"area={args.area}"
    print(f"check_struck: {len(hits)} entr{'y' if len(hits) == 1 else 'ies'} "
          f"already on record for {query!r}:\n")
    for row in hits:
        print(render(row))
    print("\nThese are measurements somebody already paid for. If you still think the")
    print("vein is live, name the entry you are overturning in your report and say why.")
    return 3


if __name__ == "__main__":
    sys.exit(main())
