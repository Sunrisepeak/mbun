#!/usr/bin/env python3
"""Diff two corpus rounds and gate on green->non-green regressions.

Each corpus round (produced by bun_corpus_runner.py / node_corpus_runner.py)
drops a `results.tsv` keyed by root-relative test path -- the stable file
identity across rounds. Comparing two rounds by hand (a python one-liner over
two TSVs) has repeatedly gone wrong: a drifted cwd loads the wrong TSV, a stale
baseline silently compares a round against itself, and a per-file regression
slips through unnoticed into a merge. This tool makes that comparison a tested,
CI-gateable operation.

"green" is the only classification the runners treat as a real pass (a file
whose every declared test passed); every other bucket -- test-failure, crash,
timeout, load-error, all-skipped, ... -- verified nothing or failed. So the
regression that matters is a file going **green -> non-green**; the tool exits
non-zero when any such regression is present (minus a --allow-regressions
manifest for known-flaky files), so a merge can be blocked on it.
"""

from __future__ import annotations

import argparse
import csv
import fnmatch
import json
from dataclasses import dataclass, field
from pathlib import Path


# The single classification the runners score as a real pass. Kept as a module
# constant so "what counts as green" has exactly one definition shared by the
# regression gate, the gain list, and the JSON summary.
GREEN = "green"


@dataclass(frozen=True)
class Move:
    """A file that stayed non-green in both rounds but moved its passed count.

    An assertion-level signal the bucket counts hide: a file stuck in
    `test-failure` that went from 100 passed assertions to 0 is a real
    regression even though its bucket never changed.
    """
    path: str
    before_passed: int
    after_passed: int

    @property
    def delta(self) -> int:
        return self.after_passed - self.before_passed


@dataclass(frozen=True)
class PerfMove:
    """A file whose wall-clock (duration_ms) grew past the perf threshold."""
    path: str
    before_ms: int
    after_ms: int

    @property
    def ratio(self) -> float:
        # before_ms is guaranteed > 0 by the caller (a floor filters noise), so
        # this never divides by zero.
        return self.after_ms / self.before_ms


@dataclass
class Diff:
    before_label: str
    after_label: str
    before_files: int
    after_files: int
    # bucket -> (before_count, after_count) over the union of both rounds.
    buckets: dict[str, tuple[int, int]]
    regressions: list[str]        # green -> non-green, gate-failing
    allowed_regressions: list[str]  # green -> non-green, excused by manifest
    gains: list[str]              # non-green -> green
    moves: list[Move]             # non-green in both, |delta passed| desc
    added: list[str]              # present only in the after round
    dropped: list[str]            # present only in the before round
    perf_moves: list[PerfMove] = field(default_factory=list)

    @property
    def gate_failed(self) -> bool:
        # Only *un-excused* green->non-green regressions fail the gate; the
        # allow-manifest path (network-flaky files) is exempt by design.
        return bool(self.regressions)


def read_list(path: Path) -> list[str]:
    """Read a manifest of fnmatch patterns, skipping blanks and `#` comments.

    Same shape and semantics as the runners' blocked/allow manifests, so an
    operator writes one kind of file for every tool in this directory.
    """
    values: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if value and not value.startswith("#"):
            values.append(value)
    return values


def resolve_results(where: Path) -> Path:
    """Accept either a round output dir or a results.tsv directly.

    The runners write `<out>/results.tsv`, so a caller almost always has the
    directory in hand; pointing straight at a TSV is supported for ad-hoc use.
    """
    if where.is_dir():
        return where / "results.tsv"
    return where


def load_round(where: Path) -> dict[str, dict[str, str]]:
    """Load one round's results.tsv into {path: row}.

    Path is the identity across rounds. A duplicate path in a single TSV is a
    corrupt round (the runners never emit one) and is rejected loudly rather
    than silently letting the last row win.
    """
    results_path = resolve_results(where)
    if not results_path.is_file():
        raise SystemExit(f"corpus_diff: no results.tsv at {results_path}")
    rows: dict[str, dict[str, str]] = {}
    with results_path.open(encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames is None or "path" not in reader.fieldnames \
                or "classification" not in reader.fieldnames:
            raise SystemExit(
                f"corpus_diff: {results_path} is missing the path/classification "
                "columns -- is it a runner results.tsv?"
            )
        for row in reader:
            path = row["path"]
            if path in rows:
                raise SystemExit(
                    f"corpus_diff: duplicate path {path!r} in {results_path}"
                )
            rows[path] = row
    return rows


def is_green(row: dict[str, str]) -> bool:
    return row["classification"] == GREEN


def to_int(value: str) -> int:
    # results.tsv is machine-written, but a truncated round (killed mid-write)
    # can leave a partial last row; treat an unparseable count as 0 rather than
    # crashing the whole diff on one bad cell.
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def compute_diff(
    before: dict[str, dict[str, str]],
    after: dict[str, dict[str, str]],
    before_label: str,
    after_label: str,
    allow_patterns: list[str] | None = None,
    perf_threshold: float | None = None,
    perf_floor_ms: int = 100,
) -> Diff:
    """Compare two loaded rounds. Pure/​deterministic -- the self-test drives it."""
    allow_patterns = allow_patterns or []

    # Per-bucket before->after counts over the union of classifications.
    buckets: dict[str, tuple[int, int]] = {}
    before_counts: dict[str, int] = {}
    after_counts: dict[str, int] = {}
    for row in before.values():
        before_counts[row["classification"]] = before_counts.get(row["classification"], 0) + 1
    for row in after.values():
        after_counts[row["classification"]] = after_counts.get(row["classification"], 0) + 1
    for bucket in sorted(set(before_counts) | set(after_counts)):
        buckets[bucket] = (before_counts.get(bucket, 0), after_counts.get(bucket, 0))

    common = set(before) & set(after)
    added = sorted(set(after) - set(before))
    dropped = sorted(set(before) - set(after))

    regressions: list[str] = []
    allowed_regressions: list[str] = []
    gains: list[str] = []
    moves: list[Move] = []
    perf_moves: list[PerfMove] = []

    for path in sorted(common):
        was_green = is_green(before[path])
        now_green = is_green(after[path])
        if was_green and not now_green:
            # An fnmatch against the manifest excuses known-flaky files (the
            # network-dependent ones) from failing the gate -- same matching the
            # runner uses for its blocked manifest.
            if any(fnmatch.fnmatch(path, pattern) for pattern in allow_patterns):
                allowed_regressions.append(path)
            else:
                regressions.append(path)
        elif now_green and not was_green:
            gains.append(path)
        elif not was_green and not now_green:
            # Stayed non-green: surface an assertion-level move if the passed
            # count changed at all (0-deltas are dropped as noise).
            delta = to_int(after[path]["passed"]) - to_int(before[path]["passed"])
            if delta != 0:
                moves.append(Move(path, to_int(before[path]["passed"]),
                                  to_int(after[path]["passed"])))

        # Optional perf signal: a file whose wall-clock grew past the threshold.
        # Only files with a meaningful `after` runtime are considered, so 1ms
        # jitter never masquerades as a perf regression.
        if perf_threshold is not None and "duration_ms" in after[path]:
            before_ms = to_int(before[path].get("duration_ms", "0"))
            after_ms = to_int(after[path].get("duration_ms", "0"))
            if before_ms > 0 and after_ms >= perf_floor_ms \
                    and after_ms >= before_ms * perf_threshold:
                perf_moves.append(PerfMove(path, before_ms, after_ms))

    # Largest absolute assertion move first; path breaks ties for determinism.
    moves.sort(key=lambda move: (-abs(move.delta), move.path))
    perf_moves.sort(key=lambda move: (-move.ratio, move.path))

    return Diff(
        before_label=before_label,
        after_label=after_label,
        before_files=len(before),
        after_files=len(after),
        buckets=buckets,
        regressions=regressions,
        allowed_regressions=allowed_regressions,
        gains=gains,
        moves=moves,
        added=added,
        dropped=dropped,
        perf_moves=perf_moves,
    )


def format_report(diff: Diff, top: int) -> str:
    """Render the human report. Deterministic text so the self-test can assert it."""
    lines: list[str] = []
    lines.append(
        f"corpus diff: {diff.before_label} -> {diff.after_label} "
        f"({diff.before_files} -> {diff.after_files} files)"
    )
    lines.append("")

    # Per-bucket before->after with signed deltas.
    name_width = max((len(bucket) for bucket in diff.buckets), default=6)
    name_width = max(name_width, len("bucket"))
    lines.append(f"{'bucket':<{name_width}}  {'before':>6}  {'after':>6}  {'delta':>6}")
    for bucket, (before_count, after_count) in diff.buckets.items():
        delta = after_count - before_count
        marker = f"{delta:+d}" if delta else "0"
        lines.append(f"{bucket:<{name_width}}  {before_count:>6}  {after_count:>6}  {marker:>6}")
    lines.append("")

    lines.append(f"REGRESSIONS (green -> non-green): {len(diff.regressions)}")
    for path in diff.regressions:
        lines.append(f"  {path}")
    if diff.allowed_regressions:
        lines.append(f"allowed regressions (excused by manifest): {len(diff.allowed_regressions)}")
        for path in diff.allowed_regressions:
            lines.append(f"  {path}")
    lines.append("")

    lines.append(f"gains (non-green -> green): {len(diff.gains)}")
    for path in diff.gains:
        lines.append(f"  {path}")
    lines.append("")

    if diff.added or diff.dropped:
        lines.append(f"files added: {len(diff.added)}  dropped: {len(diff.dropped)}")
        for path in diff.added:
            lines.append(f"  + {path}")
        for path in diff.dropped:
            lines.append(f"  - {path}")
        lines.append("")

    shown = diff.moves[:top]
    lines.append(
        f"assertion moves among files non-green in both rounds "
        f"(top {len(shown)} of {len(diff.moves)} by |delta passed|):"
    )
    for move in shown:
        lines.append(f"  {move.delta:+d}  {move.path}  "
                     f"({move.before_passed} -> {move.after_passed})")

    if diff.perf_moves:
        lines.append("")
        perf_shown = diff.perf_moves[:top]
        lines.append(
            f"perf regressions (duration_ms grew, top {len(perf_shown)} "
            f"of {len(diff.perf_moves)}):"
        )
        for move in perf_shown:
            lines.append(f"  x{move.ratio:.2f}  {move.path}  "
                         f"({move.before_ms}ms -> {move.after_ms}ms)")

    lines.append("")
    if diff.gate_failed:
        lines.append(f"GATE: FAIL -- {len(diff.regressions)} green->non-green regression(s)")
    else:
        excused = f" ({len(diff.allowed_regressions)} excused)" if diff.allowed_regressions else ""
        lines.append(f"GATE: PASS -- no un-excused regressions{excused}")
    return "\n".join(lines)


def to_json(diff: Diff, top: int) -> str:
    """Machine summary for CI consumption. Mirrors the human report's content."""
    payload = {
        "before": diff.before_label,
        "after": diff.after_label,
        "before_files": diff.before_files,
        "after_files": diff.after_files,
        "buckets": {
            bucket: {"before": before_count, "after": after_count,
                     "delta": after_count - before_count}
            for bucket, (before_count, after_count) in diff.buckets.items()
        },
        "regressions": diff.regressions,
        "allowed_regressions": diff.allowed_regressions,
        "gains": diff.gains,
        "added": diff.added,
        "dropped": diff.dropped,
        "moves": [
            {"path": move.path, "before_passed": move.before_passed,
             "after_passed": move.after_passed, "delta": move.delta}
            for move in diff.moves[:top]
        ],
        "perf_moves": [
            {"path": move.path, "before_ms": move.before_ms,
             "after_ms": move.after_ms, "ratio": round(move.ratio, 3)}
            for move in diff.perf_moves[:top]
        ],
        "gate_failed": diff.gate_failed,
    }
    return json.dumps(payload, indent=2, sort_keys=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("before", type=Path,
                        help="baseline round dir (or its results.tsv)")
    parser.add_argument("after", type=Path,
                        help="new round dir (or its results.tsv)")
    parser.add_argument(
        "--allow-regressions", type=Path,
        help="manifest of fnmatch patterns for known-flaky files (e.g. the "
             "network-dependent hosted-git-info, tcp-server) whose green->non-green "
             "regression must not fail the gate",
    )
    parser.add_argument("--json", action="store_true",
                        help="emit a machine-readable summary instead of the report")
    parser.add_argument("--top", type=int, default=15,
                        help="how many assertion/perf moves to show (default: 15)")
    parser.add_argument(
        "--perf", action="store_true",
        help="also flag files whose duration_ms grew past --perf-threshold",
    )
    parser.add_argument("--perf-threshold", type=float, default=1.5,
                        help="perf-regression growth ratio (default: 1.5 = +50%%)")
    parser.add_argument("--perf-floor-ms", type=int, default=100,
                        help="ignore perf moves whose after-runtime is below this "
                             "many ms (default: 100), so jitter is not flagged")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.top < 0:
        raise SystemExit("--top must be non-negative")
    before = load_round(args.before)
    after = load_round(args.after)
    allow_patterns = read_list(args.allow_regressions.resolve()) if args.allow_regressions else []
    diff = compute_diff(
        before, after,
        before_label=args.before.name or str(args.before),
        after_label=args.after.name or str(args.after),
        allow_patterns=allow_patterns,
        perf_threshold=args.perf_threshold if args.perf else None,
        perf_floor_ms=args.perf_floor_ms,
    )
    if args.json:
        print(to_json(diff, args.top))
    else:
        print(format_report(diff, args.top))
    # Non-zero exit gates a merge on any un-excused green->non-green regression.
    return 1 if diff.gate_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
