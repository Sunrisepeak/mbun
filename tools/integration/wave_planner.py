#!/usr/bin/env python3
"""wave_planner -- turn measured corpus state + measured lane throughput into the
next wave's assignments, and refuse to plan one the box cannot survive.

This closes the loop the campaign had been running by hand:

    full corpus runs  ->  actionable density per area
    lane_ledger.tsv   ->  observed files/hour per corpus
    struck.tsv        ->  areas already retired, with the cost that retired them
    live resources    ->  how many lanes may run at all
                       ->  ranked assignments with a goal each

Three modes:

    --coverage    where 100% actually stands: green / actionable / struck-blocked,
                  per corpus, so the un-reachable remainder is NAMED rather than
                  implied.
    --plan N      emit N ranked lane assignments (default 5, capped by resources).
    --throughput  what the ledger says about files/hour, which is what sets goals.

Why goals come from the ledger instead of a constant: measured throughput across
waves 56-57 spanned 25x (1.4 to 15.0 files/hour), and flat +6 goals were both
far too easy for zlib and out of reach for the bundler. A goal that ignores
measured yield is not a target, it is a guess.

RESOURCE GUARD. The campaign's constraint is "do not freeze the box and do not
blow memory", and both have nearly happened: the disk hit 100% twice, and a
measurement run next to four other lanes turned 113 real failures into 366
phantom ones. So this refuses to plan lanes it cannot afford, and says why.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Per-lane resource cost, measured on this box rather than guessed:
#   disk  -- a lane worktree that must build carries ~2 GB of target/ output;
#            reusing an existing worktree is much cheaper, but plan for the worst.
#   mem   -- a build peaks around 4 GB resident with ninja at full width.
DISK_GB_PER_LANE = 2
MEM_GB_PER_LANE = 4
# Headroom left untouched: below this the bounded layer starts refusing runs, and
# a run that dies on a full disk reads like a runner bug (that cost hours once).
DISK_GB_RESERVE = 12
MEM_GB_RESERVE = 8
# Hard ceiling from the campaign's own constraint, independent of resources.
MAX_LANES = 5


def read_tsv(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    if not path.exists():
        return rows
    header: list[str] | None = None
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = [p.strip() for p in line.split("\t")]
        if header is None:
            header = parts
            continue
        if len(parts) < len(header):
            parts += [""] * (len(header) - len(parts))
        rows.append(dict(zip(header, parts)))
    return rows


def load_run(run_dir: Path) -> list[tuple[str, str]]:
    """Return (path, classification) for a corpus run dir. Handles both runners:
    the node runner's results.tsv has a `status`-shaped column, the bun runner's
    has `classification` in column 7."""
    tsv = run_dir / "results.tsv"
    if not tsv.exists():
        tsv = run_dir / "results.partial.tsv"
    if not tsv.exists():
        return []
    out: list[tuple[str, str]] = []
    lines = tsv.read_text().splitlines()
    if not lines:
        return out
    header = [h.strip() for h in lines[0].split("\t")]
    try:
        cls_idx = header.index("classification")
    except ValueError:
        cls_idx = None
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        path = parts[0].strip()
        if cls_idx is not None and len(parts) > cls_idx:
            out.append((path, parts[cls_idx].strip()))
        else:
            out.append((path, parts[1].strip()))
    return out


GREEN = {"green", "pass"}
# Not actionable by a lane: the runner could not get a verdict, or the environment
# is the blocker. Counting these as "fixable" is how a subsystem looks denser than
# it is -- the http2 brief overstated fixability exactly this way.
UNVERDICTED = {"timeout", "oom-kill", "crash", "load-error", "blocked-external",
               "all-skipped", "no-tests", "skipped", "fixture-build-error",
               "ahead-of-reference"}


def area_of(path: str, corpus: str) -> str:
    """node: the test-<subsystem> prefix. bun: the two directory levels under
    compat/bun/test, which is where the real distribution lives -- grouping bun by
    FILE made 260 failures in js/bun look like 260 separate subsystems."""
    parts = path.split("/")
    if corpus == "node":
        name = parts[-1]
        bits = name.split("-")
        return "-".join(bits[:2]) if len(bits) >= 2 else name
    if len(parts) >= 5:
        return f"{parts[3]}/{parts[4]}"
    return "/".join(parts[3:]) or path


def struck_areas(struck: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    """Areas a lane should not be pointed at. COSTS_GREEN and LOW_YIELD are
    warnings, not exclusions -- a file-level retirement does not retire a whole
    subsystem. BLOCKED/TOO_BIG/NOT_A_BUG do exclude."""
    excluded: dict[str, dict[str, str]] = {}
    for row in struck:
        if row.get("verdict") in {"BLOCKED", "TOO_BIG", "NOT_A_BUG"}:
            key = row.get("target", "").lower()
            if key:
                excluded[key] = row
    return excluded


def is_struck(area: str, excluded: dict[str, dict[str, str]]) -> dict[str, str] | None:
    """Match a corpus area against a struck target.

    The subtlety that broke the first version: areas are named `test-tls` while the
    registry says "tls as a cluster target", so neither string contains the other
    and `test-tls` was ranked FIRST in a plan despite being struck as TOO_BIG.
    So compare on the area's core token (`test-` stripped) at a WORD boundary --
    substring matching here would let area `vm` match any entry merely mentioning
    "vm" somewhere in its prose.
    """
    a = area.lower().strip()
    if not a:
        return None
    core = a[5:] if a.startswith("test-") else a
    for key, row in excluded.items():
        k = key.lower()
        if a in k or k.startswith(a):
            return row
        # Require >=3 chars for the word-boundary match. A short token matches
        # ordinary English: area "a" matched "as A cluster target" and silently
        # dropped a ledger row from the throughput model.
        if len(core) >= 3 and re.search(
                rf"(?:^|[\s/(]){re.escape(core)}(?:$|[\s/),])", k):
            return row
    return None



PORT_MARKERS = ("1:1 translation", "Mechanical 1:1", "机械翻译", "bun-ref")


def builtin_shape(area: str, builtins_dir: Path) -> tuple[str, str]:
    """Is the implementation behind this area a PORT of the real source, or HAND-WRITTEN?

    This is the question that predicts yield and the one nothing in the loop was
    asking. Measured correlation: every partition declaring itself a 1:1 port of
    node/bun source is healthy (the 12 `node_stream_*` partitions -> test-stream
    237/249 = 95%), while the hand-written ones carry the long tail (node_worker 39
    actionable, node_http 37, async_hooks 31). The campaign's method for this is
    移植三段法 -- translate the real source, fix what breaks, optimise.

    THE MAPPING IS A HEURISTIC AND MUST BE SANITY-CHECKED. Naming a single file by
    substring got `test-net` badly wrong: it matched `node_net.cppm`, an 8 KB
    SocketAddress-only partition, while the real implementation is
    `js_net.cppm` + `js_net_part2.cppm` at 5,703 lines. A lane briefed on that
    mapping correctly refused the port and said so. So this now searches the whole
    `src/` tree (not just `builtins/`), ranks candidates by SIZE, and returns every
    candidate it found so the dispatcher can see when the mapping is wrong.
    """
    src_dir = builtins_dir if builtins_dir.name != "builtins" else builtins_dir.parent
    if not src_dir.is_dir():
        return ("unknown", "")
    token = area[5:] if area.startswith("test-") else area
    token = token.split("/")[0].split("+")[0]
    # 2 chars is fine here: this matches file STEMS (node_vm.cppm), not prose. The
    # >=3 guard belongs to is_struck, which greps English and would match "vm"
    # inside any sentence mentioning it.
    if len(token) < 2:
        return ("unknown", "")

    cands: list[tuple[int, Path]] = []
    for f in list(src_dir.rglob("*.cppm")) + list(src_dir.rglob("*.inc")):
        stem = f.stem
        if stem in (f"node_{token}", token, f"js_{token}") or \
                stem.startswith((f"node_{token}_", f"js_{token}_", f"{token}_")):
            cands.append((f.stat().st_size, f))
    if not cands:
        return ("unknown", "")
    cands.sort(reverse=True)
    total = sum(sz for sz, _ in cands)
    # Judge the shape from the LARGEST file -- that is the implementation. A small
    # satellite partition declaring itself a port does not make the subsystem ported.
    biggest = cands[0][1]
    head = biggest.read_text(errors="replace")[:4000]
    shape = "port" if any(m in head for m in PORT_MARKERS) else "unmarked"
    kb = total // 1024
    names = ", ".join(f"{f.name} {sz // 1024}K" for sz, f in cands[:3])
    if len(cands) > 3:
        names += f", +{len(cands) - 3} more"
    return (shape, f"{names} (total {kb}K)")


def throughput(ledger: list[dict[str, str]],
               excluded: dict[str, dict[str, str]] | None = None) -> dict[str, dict[str, float]]:
    """files/hour per corpus, from verified rows only. The mean plus the max, since
    with this few rows the max is what proves a rate is achievable.

    Rows whose AREA is now struck are excluded from the model. This is not
    flattery: those lanes measured real throughput, but they measured it against
    targets the strategy will never choose again, so including them predicts the
    wrong thing. Concretely, bun lanes D (bake/dev) and E (bundler) delivered 0 and
    1 file and both areas are now struck TOO_BIG -- leaving them in pulled bun's
    mean down to 2.0 files/hour and set a +6 goal for an area where another lane
    had just delivered +13.
    """
    stats: dict[str, dict[str, float]] = {}
    excluded = excluded or {}
    for corpus in {row.get("corpus", "") for row in ledger}:
        rows = [r for r in ledger if r.get("corpus") == corpus
                and not is_struck(r.get("area", ""), excluded)]
        if not rows:
            rows = [r for r in ledger if r.get("corpus") == corpus]
        rates = []
        for r in rows:
            try:
                mins = float(r.get("minutes", 0) or 0)
                got = float(r.get("delivered", 0) or 0)
            except ValueError:
                continue
            if mins > 0:
                rates.append(got / (mins / 60.0))
        if rates:
            stats[corpus] = {
                "lanes": float(len(rates)),
                "mean": sum(rates) / len(rates),
                "best": max(rates),
                "total_files": float(sum(float(r.get("delivered", 0) or 0) for r in rows)),
                "total_hours": sum(float(r.get("minutes", 0) or 0) for r in rows) / 60.0,
            }
    return stats


def resource_budget() -> tuple[int, list[str]]:
    """How many lanes the box can afford, and the reasons for the cap."""
    notes: list[str] = []
    usage = shutil.disk_usage(str(HERE))
    free_disk_gb = usage.free // (1024 ** 3)
    disk_lanes = max(0, (free_disk_gb - DISK_GB_RESERVE) // DISK_GB_PER_LANE)
    notes.append(f"disk: {free_disk_gb}G free, {DISK_GB_RESERVE}G reserved, "
                 f"{DISK_GB_PER_LANE}G/lane -> {disk_lanes} lanes")

    free_mem_gb = 0
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                free_mem_gb = int(line.split()[1]) // (1024 ** 2)
                break
    except OSError:
        notes.append("mem: /proc/meminfo unreadable; not constraining")
        free_mem_gb = MEM_GB_RESERVE + MEM_GB_PER_LANE * MAX_LANES
    mem_lanes = max(0, (free_mem_gb - MEM_GB_RESERVE) // MEM_GB_PER_LANE)
    notes.append(f"mem: {free_mem_gb}G available, {MEM_GB_RESERVE}G reserved, "
                 f"{MEM_GB_PER_LANE}G/lane -> {mem_lanes} lanes")

    try:
        cores = len(subprocess.run(["nproc"], capture_output=True, text=True,
                                   check=True).stdout.split()) and int(
            subprocess.run(["nproc"], capture_output=True, text=True,
                           check=True).stdout.strip())
    except Exception:
        cores = 8
    # A lane measures at --jobs 3-4; leave the box able to also run a full-corpus
    # verification. Measuring under contention is a documented trap.
    cpu_lanes = max(1, (cores - 6) // 4)
    notes.append(f"cpu: {cores} cores, reserving 6 for verification, "
                 f"~4/lane -> {cpu_lanes} lanes")

    allowed = min(MAX_LANES, disk_lanes, mem_lanes, cpu_lanes)
    notes.append(f"campaign ceiling: {MAX_LANES} lanes")
    return int(allowed), notes


def summarise(run: list[tuple[str, str]], corpus: str,
              excluded: dict[str, dict[str, str]]):
    per_area: dict[str, dict[str, int]] = {}
    for path, cls in run:
        area = area_of(path, corpus)
        bucket = per_area.setdefault(area, {"green": 0, "actionable": 0, "unverdicted": 0})
        if cls in GREEN:
            bucket["green"] += 1
        elif cls in UNVERDICTED:
            bucket["unverdicted"] += 1
        else:
            bucket["actionable"] += 1
    for area, bucket in per_area.items():
        row = is_struck(area, excluded)
        bucket["struck"] = 1 if row else 0
        bucket["_verdict"] = row.get("verdict", "") if row else ""
        bucket["_why"] = row.get("cost_or_size", "") if row else ""
    return per_area


def cmd_coverage(args, node_run, bun_run, excluded):
    print("=== coverage: where 100% actually stands ===\n")
    grand = {}
    for corpus, run in (("node", node_run), ("bun", bun_run)):
        if not run:
            print(f"{corpus}: no run data\n")
            continue
        per_area = summarise(run, corpus, excluded)
        total = len(run)
        green = sum(b["green"] for b in per_area.values())
        unv = sum(b["unverdicted"] for b in per_area.values())
        struck_fail = sum(b["actionable"] for b in per_area.values() if b["struck"])
        actionable = sum(b["actionable"] for b in per_area.values() if not b["struck"])
        grand[corpus] = (green, actionable, struck_fail, unv, total)
        pct = 100.0 * green / total if total else 0.0
        print(f"{corpus}: {green}/{total} green ({pct:.1f}%)")
        print(f"  actionable failures      {actionable}")
        print(f"  in struck/blocked areas  {struck_fail}")
        print(f"  no verdict (timeout/oom/skip/env) {unv}")
        reachable = green + actionable
        print(f"  => ceiling if every actionable file lands: "
              f"{reachable}/{total} ({100.0 * reachable / total:.1f}%)\n")
    if len(grand) == 2:
        g = sum(v[0] for v in grand.values())
        t = sum(v[4] for v in grand.values())
        a = sum(v[1] for v in grand.values())
        print(f"both corpora: {g}/{t} green ({100.0 * g / t:.1f}%), "
              f"{a} actionable failures remaining")
        print("100% is NOT reachable by lane work alone: the struck and "
              "no-verdict columns above name what blocks it.")
    return 0


def cmd_throughput(args, ledger, excluded):
    stats = throughput(ledger, excluded)
    if not stats:
        print("wave_planner: ledger has no usable rows", file=sys.stderr)
        return 2
    print("=== measured lane throughput (verified rows only) ===\n")
    for corpus, s in sorted(stats.items()):
        print(f"{corpus}: {int(s['lanes'])} lanes, {int(s['total_files'])} files "
              f"in {s['total_hours']:.1f} lane-hours")
        print(f"  mean {s['mean']:.1f} files/hour, best {s['best']:.1f}")
    print("\nGoals are set from the mean, capped by an area's actionable count.")
    print("The best column matters: it proves a rate is achievable, so a lane far")
    print("below it is a method problem, not a subsystem problem.")
    return 0


def cmd_plan(args, node_run, bun_run, ledger, excluded):
    allowed, notes = resource_budget()
    print("=== resource guard ===")
    for n in notes:
        print(f"  {n}")
    want = args.plan
    lanes = min(want, allowed)
    print(f"  => planning {lanes} lane(s) (asked for {want})\n")
    if lanes == 0:
        print("wave_planner: REFUSING to plan any lane -- free the resources above "
              "first (tools/integration/reclaim_disk.sh --prune-configs --apply).")
        return 1

    stats = throughput(ledger, excluded)
    candidates = []
    for corpus, run in (("node", node_run), ("bun", bun_run)):
        if not run:
            continue
        rate = stats.get(corpus, {}).get("mean", 4.0)
        for area, b in summarise(run, corpus, excluded).items():
            if b["struck"] or b["actionable"] < args.min_actionable:
                continue
            if any(x.lower() in area.lower() for x in args.exclude):
                continue
            # Expected yield for a ~2h lane, never more than what is there to fix.
            expected = min(b["actionable"], rate * args.lane_hours)
            shape, impl = builtin_shape(area, args.builtins) if corpus == "node" \
                else ("unknown", "")
            # A hand-written subsystem is a PORT candidate, and porting has measured
            # ~3x the yield of fixing. Rank it above an equally dense ported area.
            weight = 1.5 if shape == "unmarked" else 1.0
            candidates.append({
                "corpus": corpus, "area": area, "green": b["green"],
                "actionable": b["actionable"], "unverdicted": b["unverdicted"],
                "rate": rate, "goal": max(1, int(expected * 0.75)),
                "expected": expected * weight, "shape": shape, "impl": impl,
            })
    candidates.sort(key=lambda c: (-c["expected"], -c["actionable"]))

    # Ranking by expected yield alone starves the slower corpus: node's measured
    # rate is ~3x bun's, so a pure sort hands every lane to node and the bun metric
    # stops moving. Both corpora are first-class targets, so each gets a floor.
    chosen: list[dict] = []
    if args.min_per_corpus > 0:
        for corpus in ("node", "bun"):
            picked = [c for c in candidates if c["corpus"] == corpus][:args.min_per_corpus]
            chosen.extend(picked)
        chosen.sort(key=lambda c: (-c["expected"], -c["actionable"]))
        chosen = chosen[:lanes]
        for c in candidates:
            if len(chosen) >= lanes:
                break
            if c not in chosen:
                chosen.append(c)
    else:
        chosen = candidates[:lanes]

    print("=== ranked assignments ===\n")
    for i, c in enumerate(chosen, 1):
        print(f"{i}. [{c['corpus']}] {c['area']}")
        print(f"     green {c['green']} / actionable {c['actionable']} "
              f"/ no-verdict {c['unverdicted']}")
        print(f"     corpus rate {c['rate']:.1f} files/h x {args.lane_hours}h "
              f"-> GOAL +{c['goal']}")
        if c.get("shape") == "unmarked":
            print(f"     shape: NO PORT MARKER ({c['impl']})")
            # Size is the signal for WHICH port shape. A wholesale port of a large
            # subsystem has no safe partial landing -- a half-ported net.Socket is
            # 121 files red plus http/https/http2 -- and one lane sized exactly that
            # at 3-5 lanes before declining it. What it did instead, and where its
            # +19 came from, was porting node's algorithms function-by-function into
            # the existing structure. That is the right default above ~150K.
            kb = 0
            for part in c["impl"].replace("(", " ").replace(")", " ").split():
                if part.endswith("K") and part[:-1].isdigit():
                    kb = max(kb, int(part[:-1]))
            big = "total" in c["impl"] and kb >= 150
            # A missing marker does NOT mean hand-written. node_http.cppm's
            # OutgoingMessage/IncomingMessage/Agent halves are near-verbatim node and
            # simply never declared it; a lane briefed as "hand-written, re-port it"
            # would have burned itself for ~0 files. It instead ran a FIDELITY AUDIT
            # against compat/node/lib first, found the gaps were narrow
            # (_http_common, internal/http, the client socket loop, a handful of
            # omitted functions) and landed +17 at 10.4 files/hour. Audit first.
            print(f"     -> AUDIT FIRST: diff this against compat/node/lib/ before "
                  f"committing to a port. A missing marker means UNKNOWN fidelity, not "
                  f"hand-written -- some partitions are near-verbatim node undeclared.")
            if big:
                print(f"     -> if the audit finds it genuinely divergent: port node's "
                      f"algorithms FUNCTION-BY-FUNCTION into the existing structure. At "
                      f"this size a wholesale port has no safe partial landing (3-5 lanes).")
            else:
                print(f"     -> if the audit finds it genuinely divergent: 移植三段法 "
                      f"-- translate the real source, fix what breaks, then measure.")
            print(f"     -> VERIFY the shadowing hypothesis before relying on it: probe "
                  f"the prototype chain. It only pays when hand-rolled methods sit ON a "
                  f"correctly-ported base class.")
        elif c.get("shape") == "port":
            print(f"     shape: already a 1:1 port ({c['impl']}) -> fix-shaped lane is "
                  f"appropriate here")
    by_corpus: dict[str, int] = {}
    for c in chosen:
        by_corpus[c["corpus"]] = by_corpus.get(c["corpus"], 0) + 1
    print(f"\nper-corpus split: " +
          ", ".join(f"{k} {v}" for k, v in sorted(by_corpus.items())) +
          f" (floor {args.min_per_corpus}/corpus)")
    rest = [c for c in candidates if c not in chosen]
    if rest:
        print(f"(next in line, not dispatched: "
              f"{', '.join(c['area'] + '[' + c['corpus'] + ']' for c in rest[:4])})")
    print("\nBefore dispatching each: python3 tools/integration/check_struck.py <area terms>")
    print("Every brief must carry: diff against a FROZEN baseline (never rebuild to")
    print("make one), read compat/node/lib/, sort candidates by assertion ratio, and")
    print("re-run any deciding file serially before believing it.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--node-run", type=Path, help="a node corpus run dir")
    ap.add_argument("--bun-run", type=Path, help="a bun corpus run dir")
    ap.add_argument("--ledger", type=Path, default=HERE / "lane_ledger.tsv")
    ap.add_argument("--struck", type=Path, default=HERE / "struck.tsv")
    ap.add_argument("--coverage", action="store_true")
    ap.add_argument("--throughput", action="store_true")
    ap.add_argument("--plan", type=int, nargs="?", const=5, default=None,
                    help="emit N ranked lane assignments (default 5)")
    ap.add_argument("--lane-hours", type=float, default=2.0)
    ap.add_argument("--min-actionable", type=int, default=8,
                    help="ignore areas with fewer actionable failures than this")
    ap.add_argument("--builtins", type=Path,
                    default=HERE.parent.parent / "modules/jsc/src/builtins",
                    help="where to look up whether a subsystem is ported or hand-written")
    ap.add_argument("--min-per-corpus", type=int, default=2,
                    help="floor of lanes per corpus, so the slower one is not starved")
    ap.add_argument("--exclude", action="append", default=[],
                    help="area substring to skip (e.g. one mined last wave); repeatable")
    args = ap.parse_args()

    if not (args.coverage or args.throughput or args.plan is not None):
        ap.error("pick one of --coverage, --throughput, --plan")

    ledger = read_tsv(args.ledger)
    excluded = struck_areas(read_tsv(args.struck))
    node_run = load_run(args.node_run) if args.node_run else []
    bun_run = load_run(args.bun_run) if args.bun_run else []

    if args.throughput:
        return cmd_throughput(args, ledger, excluded)
    if args.coverage:
        if not (node_run or bun_run):
            ap.error("--coverage needs --node-run and/or --bun-run")
        return cmd_coverage(args, node_run, bun_run, excluded)
    return cmd_plan(args, node_run, bun_run, ledger, excluded)


if __name__ == "__main__":
    sys.exit(main())
