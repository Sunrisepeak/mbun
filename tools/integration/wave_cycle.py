#!/usr/bin/env python3
"""wave_cycle -- close the loop: evaluate the wave that finished, then plan the next.

    python3 tools/integration/wave_cycle.py --evaluate 65     # what actually happened
    python3 tools/integration/wave_cycle.py --plan            # what to dispatch now
    python3 tools/integration/wave_cycle.py --evaluate 65 --plan --commit-state

WHY THIS EXISTS. The campaign had every input a strategy needs and no strategy:
`wave_planner` modelled throughput, `blocker_rank` measured how close files are,
`dispatch_gate` measured machine capacity, `lane_ledger.tsv` recorded what each
lane actually delivered -- and a human joined them by hand every round. So the
loop never closed. Nothing re-read the ledger after a wave to notice that the
goals had been wrong; nothing noticed that four consecutive waves picked targets
by area while the returns collapsed; the switch to ranking by remaining blockers
was a person changing their mind, not the system correcting itself.

This is that missing step, and it is deliberately mechanical. It does not choose
targets by taste -- it reports what the measurements imply and writes the
conclusion to `strategy_state.json`, which the next `--plan` reads back. A human
still dispatches, and should still overrule it; the point is that overruling is
now visible as a disagreement with a recorded number instead of the only thing
that ever happens.

WHAT IT COMPUTES.

  goal accuracy   delivered / goal per lane. Persistently < 1 means the goals are
                  set from a rate the work no longer supports. Wave 64 planned
                  +7/+6 per lane against a measured 5.1 files/h and delivered
                  +2 across five lanes; nothing flagged it.

  mode            per corpus, from blocker_rank's near-green pool:
                    AREA       -- a large pool of files at <= 1 blocker; ordinary
                                  file-by-file lanes pay.
                    STRUCTURAL -- the pool is small relative to the non-green
                                  count, so most remaining files need 2+ fixes
                                  and a lane that clears one blocker lands 0
                                  files. Measured: node 190/374 near-green (AREA),
                                  bun 78/924 (STRUCTURAL) -- which is exactly why
                                  bun lanes kept returning zero while node's did
                                  not, and neither the planner nor anyone else
                                  had noticed.

  capacity        from dispatch_gate, so a plan is never emitted for more lanes
                  than the box can carry.

WHAT IT DOES NOT DO. It cannot tell you WHICH file to work on -- blocker_rank
--list-near does that, and its signatures are heuristics. It has no opinion about
correctness. And a mode of STRUCTURAL is not an instruction to stop working on a
corpus; it means file counts are the wrong success metric for it that round, and
the lane should be briefed to fix a shared cause and report blocker-depth
movement instead.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
STATE = HERE / "strategy_state.json"
LEDGER = HERE / "lane_ledger.tsv"

# A corpus whose near-green pool is below this fraction of its non-green files
# cannot be worked file-by-file: most files need more than one fix, so a lane
# that lands a correct fix still reports zero. Chosen from the measured split
# (node 0.51 pays, bun 0.084 does not); anything in between is a judgement call
# the report states rather than hides.
STRUCTURAL_BELOW = 0.20


def read_ledger() -> list[dict[str, str]]:
    rows, header = [], None
    for line in LEDGER.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if header is None:
            header = parts
            continue
        if len(parts) >= len(header):
            rows.append(dict(zip(header, parts)))
    return rows


def evaluate(wave: str) -> dict:
    rows = [r for r in read_ledger() if r.get("wave") == wave]
    if not rows:
        raise SystemExit(f"wave_cycle: no ledger rows for wave {wave}")

    lanes, tot_goal, tot_got, tot_min = [], 0.0, 0.0, 0.0
    for r in rows:
        try:
            goal = float(r.get("goal", 0) or 0)
            got = float(r.get("delivered", 0) or 0)
            mins = float(r.get("minutes", 0) or 0)
        except ValueError:
            continue
        lanes.append({
            "lane": r.get("lane", "?"), "corpus": r.get("corpus", "?"),
            "area": r.get("area", "?"), "goal": goal, "delivered": got,
            "minutes": mins,
            "rate": (got / (mins / 60.0)) if mins > 0 else None,
            "accuracy": (got / goal) if goal > 0 else None,
        })
        tot_goal += goal
        tot_got += got
        tot_min += mins

    # Goal accuracy is computed ONLY over lanes that carried a numeric goal.
    # Mixing in goal-free lanes -- census, re-audit, mechanism work, all briefed
    # deliberately without a file target because "0 files is a successful lane"
    # -- makes the aggregate meaningless: wave 65 read as 4.50 (27 delivered
    # against 6 planned) purely because five of six lanes had goal 0. A ratio
    # that flatters the strategy by counting unplanned wins is worse than none.
    goaled = [l for l in lanes if l["goal"] > 0]
    g_goal = sum(l["goal"] for l in goaled)
    g_got = sum(l["delivered"] for l in goaled)
    acc = [l["accuracy"] for l in goaled if l["accuracy"] is not None]
    return {
        "wave": wave,
        "lanes": lanes,
        "goaled_lanes": len(goaled),
        "unplanned_delivered": tot_got - g_got,
        "total_goal": g_goal,
        "total_delivered": tot_got,
        "goaled_delivered": g_got,
        "total_hours": tot_min / 60.0,
        "goal_accuracy": (g_got / g_goal) if g_goal else None,
        "lane_accuracy_median": (sorted(acc)[len(acc) // 2] if acc else None),
    }


# Full corpus sizes, so a plan computed from a SUBSET run says so. A targeted
# gate is the normal artifact of a wave, and reading a mode off one silently
# treats "the files this change could reach" as "the files that remain".
CORPUS_SIZE = {"node": 4433, "bun": 1902}


def run_size(run: Path) -> int:
    try:
        return max(0, len(run.joinpath("results.tsv").read_text().splitlines()) - 1)
    except OSError:
        return 0


def near_green(run: Path, corpus: str) -> tuple[int, int] | None:
    """(files at <=1 blocker, non-green files) via blocker_rank, or None."""
    if not (run / "results.tsv").exists():
        return None
    try:
        out = subprocess.run(
            [sys.executable, str(HERE / "blocker_rank.py"),
             "--run", str(run), "--corpus", corpus],
            capture_output=True, text=True, timeout=900).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    nong = nearn = None
    for line in out.splitlines():
        s = line.strip()
        if "non-green files" in s and nong is None:
            nong = int(s.split()[0])
        if "blocker(s) -- these are what a fix can convert" in s and nearn is None:
            nearn = int(s.split()[0])
    return (nearn, nong) if (nearn is not None and nong is not None) else None


def area_rates(rows: list[dict[str, str]]) -> dict[str, dict]:
    """Measured files/hour per AREA, not per corpus.

    The corpus mean hides the thing that actually decides a goal. Wave 66 ran two
    node lanes against the same near-green list and the same +6 goal: test-tls
    delivered 6 in 1.0h (6.0 files/h, goal accuracy 1.00) and test-http2 delivered
    2 in 2.6h (0.8 files/h, 0.33). That is not lane quality -- tls's near-green
    files shared ONE missing feature (PKCS#12), http2's were independent deep
    defects. Sizing both from the corpus mean of 3.5 guarantees one goal is
    fantasy and the other is timid, and blocker_rank cannot see the difference
    because it counts how many blockers remain, not whether they share a cause.
    """
    by: dict[str, list[tuple[float, float]]] = {}
    for r in rows:
        try:
            got, mins = float(r.get("delivered", 0) or 0), float(r.get("minutes", 0) or 0)
        except ValueError:
            continue
        if mins <= 0:
            continue
        by.setdefault(r.get("area", "?"), []).append((got, mins))
    out = {}
    for area, obs in by.items():
        hours = sum(m for _, m in obs) / 60.0
        files = sum(g for g, _ in obs)
        out[area] = {"lanes": len(obs), "files": files, "hours": round(hours, 1),
                     "rate": round(files / hours, 2) if hours else None}
    return out


def mode_for(nearn: int, nong: int) -> tuple[str, float]:
    frac = (nearn / nong) if nong else 0.0
    return ("AREA" if frac >= STRUCTURAL_BELOW else "STRUCTURAL"), frac


def capacity() -> tuple[int, list[str]]:
    sys.path.insert(0, str(HERE))
    import dispatch_gate  # noqa: E402
    return dispatch_gate.capacity()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--evaluate", metavar="WAVE", help="evaluate a finished wave from the ledger")
    ap.add_argument("--plan", action="store_true", help="emit the next dispatch from measured state")
    ap.add_argument("--node-run", type=Path, help="latest node run dir")
    ap.add_argument("--bun-run", type=Path, help="latest bun run dir")
    ap.add_argument("--areas", action="store_true",
                    help="measured files/hour per area, for sizing the next goals")
    ap.add_argument("--commit-state", action="store_true",
                    help="write strategy_state.json so the next --plan reads it back")
    args = ap.parse_args()
    if not args.evaluate and not args.plan and not args.areas:
        ap.error("nothing to do: pass --evaluate WAVE, --areas and/or --plan")

    state: dict = {}
    if STATE.exists():
        try:
            state = json.loads(STATE.read_text())
        except (OSError, ValueError):
            state = {}

    if args.evaluate:
        ev = evaluate(args.evaluate)
        print(f"=== wave {ev['wave']}: what actually happened ===\n")
        print(f"  {'lane':<6} {'corpus':<6} {'area':<22} {'goal':>5} {'got':>5} "
              f"{'hours':>6} {'files/h':>8} {'goal acc':>9}")
        for l in ev["lanes"]:
            rate = f"{l['rate']:.1f}" if l["rate"] is not None else "-"
            acc = f"{l['accuracy']:.2f}" if l["accuracy"] is not None else "-"
            print(f"  {l['lane']:<6} {l['corpus']:<6} {l['area'][:22]:<22} "
                  f"{l['goal']:>5.0f} {l['delivered']:>5.0f} {l['minutes']/60:>6.1f} "
                  f"{rate:>8} {acc:>9}")
        ga = ev["goal_accuracy"]
        print(f"\n  {ev['total_delivered']:.0f} files delivered in "
              f"{ev['total_hours']:.1f} lane-hours")
        if ev["goaled_lanes"]:
            print(f"  of which PLANNED: {ev['goaled_delivered']:.0f} against "
                  f"{ev['total_goal']:.0f} across {ev['goaled_lanes']} lane(s) with a goal"
                  + (f"  -> goal accuracy {ga:.2f}" if ga is not None else ""))
        if ev["unplanned_delivered"]:
            print(f"  and UNPLANNED: {ev['unplanned_delivered']:.0f} from goal-free lanes "
                  f"(census / re-audit / mechanism). Not evidence the goals were right.")
        if ga is not None and ga < 0.5:
            print("  => goals were set from a rate the work no longer supports. The next")
            print("     plan must be sized from the MEASURED rate, not the previous target.")
        state["last_evaluated_wave"] = ev["wave"]
        state["goal_accuracy"] = ga

    if args.areas:
        rates = area_rates(read_ledger())
        ranked = sorted((a for a in rates.items() if a[1]["rate"] is not None),
                        key=lambda kv: -kv[1]["rate"])
        print("\n=== measured rate per AREA (size the next goal from the area, not the corpus) ===\n")
        print(f"  {'area':<26} {'lanes':>5} {'files':>6} {'hours':>6} {'files/h':>8}")
        for area, s in ranked[:20]:
            print(f"  {area[:26]:<26} {s['lanes']:>5} {s['files']:>6.0f} "
                  f"{s['hours']:>6.1f} {s['rate']:>8.2f}")
        print("\n  A goal is rate x hours x 0.75. An area with no row yet has no measured")
        print("  rate -- use the corpus mean and say that is what you did.")
        state["area_rates"] = {a: s for a, s in rates.items()}

    if args.plan:
        print("\n=== what the measurements imply for the next wave ===\n")
        allowed, reasons = capacity()
        for r in reasons:
            print("  " + r)
        print(f"\n  capacity: {allowed} lane(s)")

        modes = {}
        for corpus, run in (("node", args.node_run), ("bun", args.bun_run)):
            if not run:
                continue
            ng = near_green(run, corpus)
            if not ng:
                print(f"  {corpus}: no usable run at {run}")
                continue
            nearn, nong = ng
            mode, frac = mode_for(nearn, nong)
            size, full = run_size(run), CORPUS_SIZE.get(corpus, 0)
            partial = full and size < full * 0.9
            modes[corpus] = {"mode": mode, "near_green": nearn, "non_green": nong,
                             "fraction": round(frac, 3), "run_files": size,
                             "partial_run": bool(partial)}
            print(f"  {corpus}: {nearn}/{nong} non-green files at <=1 blocker "
                  f"({frac:.0%}) -> {mode}")
            if partial:
                print(f"      NOTE: computed from a {size}-file run, not the full {full}. "
                      f"A targeted gate covers what a change could REACH, not what")
                print(f"      REMAINS -- re-run the mode off a full-corpus run before")
                print(f"      trusting it to redirect a wave.")
            if mode == "AREA":
                print(f"      file-by-file lanes pay here. Target list: "
                      f"blocker_rank.py --run {run} --corpus {corpus} --list-near")
            else:
                print(f"      most remaining files need 2+ fixes, so a lane that clears one")
                print(f"      blocker lands 0 files. Brief for a SHARED CAUSE and score on")
                print(f"      blocker-depth movement, not on green-file count.")
        if modes:
            state["modes"] = modes

    if args.commit_state:
        STATE.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
        print(f"\n  state written to {STATE.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
