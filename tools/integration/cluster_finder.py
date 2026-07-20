#!/usr/bin/env python3
"""cluster_finder — turn a corpus run into a ranked, actionable work-list.

A raw `results.tsv` tells you *which* files fail; it does not tell you *why* or
*what to fix first*. Each compat round starts the same way by hand: pick a
subsystem, run its files, eyeball the error lines, notice that N of them share
one root cause, and fix that cause once for N greens. This tool does that
bucketing mechanically and repeatably, so a round targets the densest
single-root-cause cluster instead of a scattered guess.

It does NOT execute anything — it reads the output an existing corpus run
already produced (`node_corpus_runner.py --out <dir>` writes `results.tsv` plus
per-file `logs/*.log`). All bounded/systemd-scoped execution stays in the
runner (see bounded_run.py); this is pure post-hoc analysis, so it can never
freeze the machine and needs no safety layer of its own.

For each non-green, non-timeout file it distils the log into a normalised
*signature* (the error class + message with volatile bits — paths, numbers,
quoted literals, hex — replaced by placeholders) so that "cannot find module
/abs/a" and "cannot find module /abs/b" collapse to one bucket. Files are then
grouped by (subsystem, signature) and subsystems ranked by how many *fixable*
(non-timeout, non-oom) files they carry — timeouts are usually child_process
harness gaps, not a shared code bug, so they are reported separately and never
inflate a cluster's fixable score.

Usage:
    # after: node_corpus_runner.py --bin <mbun> --out /tmp/run1
    cluster_finder.py --results /tmp/run1
    cluster_finder.py --results /tmp/run1 --filter test-vm --top 15
    cluster_finder.py --results /tmp/run1 --json > clusters.json

Exit code is always 0 (analysis, not a gate) unless the results dir is unusable.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

# Error-class prefixes worth anchoring a signature on. Order matters only for
# display; membership is what selects the anchor line from a noisy log.
ERROR_ANCHORS = (
    "AssertionError", "TypeError", "RangeError", "ReferenceError", "SyntaxError",
    "Error:", "error:", "Unhandled promise rejection", "uncaughtException",
    "panic", "Segmentation fault", "not a function", "is not defined",
)


def subsystem_of(filename: str) -> str:
    """`test-vm-context.js` -> `vm`; `test-url-parse-invalid-input.js` -> `url`.

    Uses the token after `test-`, which is how the node corpus namespaces its
    files. Falls back to the whole stem when the name has no dash.
    """
    stem = Path(filename).name
    match = re.match(r"test-([a-z0-9]+)", stem)
    return match.group(1) if match else stem


def normalize_signature(log_text: str) -> str:
    """Collapse a log into a stable, low-cardinality failure signature.

    Picks the first line containing a known error anchor, then scrubs the
    volatile substrings that would otherwise split one root cause across many
    buckets: absolute paths, digit runs, quoted literals, and hex blobs.
    """
    anchor_line = ""
    for raw in log_text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if any(anchor in line for anchor in ERROR_ANCHORS):
            anchor_line = line
            break
    if not anchor_line:
        # No recognisable error line — use the last non-empty line as a weak
        # signal (often a bare assertion value or a stack frame).
        tail = [l.strip() for l in log_text.splitlines() if l.strip()]
        anchor_line = tail[-1] if tail else "<no output>"
    scrubbed = anchor_line
    scrubbed = re.sub(r"/[^\s'\"]+", "<path>", scrubbed)          # absolute paths
    scrubbed = re.sub(r"0x[0-9a-fA-F]+", "<hex>", scrubbed)       # hex addresses
    scrubbed = re.sub(r"\b\d+\b", "<n>", scrubbed)                # digit runs
    scrubbed = re.sub(r"(['\"])(?:\\.|[^\\])*?\1", "<str>", scrubbed)  # quoted literals
    # Unify placeholder variants, then collapse a run of placeholders (with any
    # short junk between them — e.g. a bare filename leaking out of nested
    # quotes in a JSC error message) into one token, so the same root cause
    # buckets together regardless of the volatile argument it printed.
    scrubbed = re.sub(r"<(?:str|path|n|hex)>", "<v>", scrubbed)
    scrubbed = re.sub(r"(?:<v>[^<]*){2,}", "<v>", scrubbed)
    scrubbed = re.sub(r"\s+", " ", scrubbed).strip()
    return scrubbed[:160]


@dataclass
class Cluster:
    subsystem: str
    signature: str
    files: list[str] = field(default_factory=list)


@dataclass
class SubsystemStat:
    subsystem: str
    green: int = 0
    fixable: int = 0        # fail (not timeout/oom)
    timeout: int = 0
    oom: int = 0
    files: list[str] = field(default_factory=list)


def load_results(results_dir: Path) -> list[dict]:
    tsv = results_dir / "results.tsv"
    if not tsv.is_file():
        raise SystemExit(f"no results.tsv under {results_dir}")
    rows = []
    with tsv.open(encoding="utf-8") as stream:
        header = stream.readline().rstrip("\n").split("\t")
        for line in stream:
            values = line.rstrip("\n").split("\t")
            if len(values) != len(header):
                continue
            rows.append(dict(zip(header, values)))
    return rows


def analyze(results_dir: Path, name_filter: str | None) -> tuple[list[SubsystemStat], list[Cluster]]:
    rows = load_results(results_dir)
    subsystems: dict[str, SubsystemStat] = {}
    clusters: dict[tuple[str, str], Cluster] = {}
    for row in rows:
        path = row.get("path", "")
        name = Path(path).name
        if name_filter and name_filter not in name:
            continue
        classification = row.get("classification", "")
        sub = subsystem_of(name)
        stat = subsystems.setdefault(sub, SubsystemStat(sub))
        stat.files.append(name)
        if classification == "pass":
            stat.green += 1
            continue
        if classification == "timeout":
            stat.timeout += 1
            continue
        if classification == "oom-kill":
            stat.oom += 1
            continue
        # A fixable failure: bucket it by its log signature.
        stat.fixable += 1
        log_rel = row.get("log", "")
        signature = "<no log>"
        if log_rel:
            log_path = results_dir / log_rel
            if log_path.is_file():
                signature = normalize_signature(
                    log_path.read_text(encoding="utf-8", errors="replace"))
        key = (sub, signature)
        clusters.setdefault(key, Cluster(sub, signature)).files.append(name)
    ranked_subsystems = sorted(
        subsystems.values(), key=lambda s: (-s.fixable, -s.timeout, s.subsystem))
    ranked_clusters = sorted(
        clusters.values(), key=lambda c: (-len(c.files), c.subsystem, c.signature))
    return ranked_subsystems, ranked_clusters


def print_report(subsystems: list[SubsystemStat], clusters: list[Cluster], top: int) -> None:
    print("== subsystems by fixable-fail density (fixable excludes timeouts/oom) ==")
    print(f"{'subsystem':16} {'green':>6} {'fixable':>8} {'timeout':>8} {'oom':>5}")
    for stat in subsystems:
        if stat.fixable == 0 and stat.timeout == 0 and stat.oom == 0:
            continue
        print(f"{stat.subsystem:16} {stat.green:>6} {stat.fixable:>8} {stat.timeout:>8} {stat.oom:>5}")
    print()
    print(f"== top {top} single-root-cause clusters (same subsystem + error signature) ==")
    for cluster in clusters[:top]:
        print(f"[{len(cluster.files):>2}] {cluster.subsystem}: {cluster.signature}")
        for name in sorted(cluster.files)[:8]:
            print(f"       {name}")
        if len(cluster.files) > 8:
            print(f"       … +{len(cluster.files) - 8} more")


def to_json(subsystems: list[SubsystemStat], clusters: list[Cluster], top: int) -> str:
    return json.dumps({
        "subsystems": [
            {"subsystem": s.subsystem, "green": s.green, "fixable": s.fixable,
             "timeout": s.timeout, "oom": s.oom}
            for s in subsystems if (s.fixable or s.timeout or s.oom)
        ],
        "clusters": [
            {"subsystem": c.subsystem, "signature": c.signature,
             "count": len(c.files), "files": sorted(c.files)}
            for c in clusters[:top]
        ],
    }, indent=2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--results", required=True, type=Path,
                        help="a node_corpus_runner --out dir (results.tsv + logs/)")
    parser.add_argument("--filter", default=None,
                        help="only files whose name contains this substring (e.g. test-vm)")
    parser.add_argument("--top", type=int, default=20, help="how many clusters to show")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    subsystems, clusters = analyze(args.results.resolve(), args.filter)
    if args.json:
        print(to_json(subsystems, clusters, args.top))
    else:
        print_report(subsystems, clusters, args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
