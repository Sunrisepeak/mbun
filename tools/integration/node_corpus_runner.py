#!/usr/bin/env python3
"""Run Node's test/parallel corpus directly through one mbun binary.

Node's upstream tests are plain scripts that expect exit code 0 (no test
runner): a file passes when mbun executes it to a clean exit. Harness
requirements node's own runner would provide (// Flags: comments, internal
bindings, python harness env) are NOT emulated — files needing them count as
failures, which keeps the measurement honest file-level coverage, comparable
across runs, rather than an API checklist.

A file that skipped itself (`common.skip()` -> `1..0 # Skipped:`, exit 0) is
classified `skipped`, never `pass`: it exits clean precisely because the
runtime lacks the feature it wanted to test, so counting it as coverage would
reward the gap it is reporting.

Each test runs through bounded_run.BoundedRun — the shared safety layer
(systemd scope limits, own session, file-backed output, private TMPDIR,
disk-headroom guard) that keeps a hostile workload from freezing the machine
or killing the harness.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from bounded_run import BoundedRun, ensure_disk_headroom

# node's common.skip() prints a TAP plan of zero tests and exits 0 (see
# test/common/index.js printSkipMessage). Exit code alone therefore cannot tell
# "ran everything and passed" from "declined to run anything", and 1527 corpus
# files can take a skip path — gated on process.features.*, hasCrypto,
# hasInspector, a missing service, or a platform. Counting those as passes
# inflates the headline by whatever the runtime happens NOT to implement: the
# 237 test-quic-* files all skip on `!process.features.quic`, so a runtime that
# never adds QUIC would bank +237 "passes" for it. The bun runner already keeps
# an `all-skipped` bucket for the same reason; this is its node counterpart.
TAP_SKIP_RE = re.compile(r"(?m)^\s*1\.\.0\s*#\s*Skipped:")


@dataclass(frozen=True)
class Result:
    path: str
    exit_code: int
    classification: str
    duration_ms: int
    log: str


def log_name(path: str) -> str:
    digest = hashlib.sha1(path.encode("utf-8"), usedforsecurity=False).hexdigest()[:12]
    readable = re.sub(r"[^A-Za-z0-9_.-]+", "_", path)[-96:]
    return f"{digest}-{readable}.log"


def run_one(binary: Path, root: Path, output_dir: Path, timeout: float, path: str, thread_id: int) -> Result:
    started = time.monotonic()
    relative_log = Path("logs") / log_name(path)
    # Node's own runner sets TEST_THREAD_ID per worker so per-file temp
    # artifacts (common.tmpDir -> .tmp.<id>) never collide. We give each job a
    # distinct id for the same reason, otherwise files that share one temp dir
    # spuriously fail with EEXIST on `.tmp.0`.
    env = dict(os.environ)
    env["TEST_THREAD_ID"] = str(thread_id)
    bounded = BoundedRun(
        output_dir / relative_log,
        private_tmp=output_dir / "tmp" / log_name(path)[:12],
    ).run([str(binary), str((root / path).resolve())], timeout=timeout, cwd=root, env=env)
    duration_ms = round((time.monotonic() - started) * 1000)
    if bounded.timed_out:
        classification = "timeout"
    elif bounded.oom_killed:
        classification = "oom-kill"
    elif bounded.exit_code == 0:
        classification = "skipped" if declared_skip(output_dir / relative_log) else "pass"
    else:
        classification = "fail"
    return Result(path, bounded.exit_code, classification, duration_ms, str(relative_log))


def declared_skip(log_path: Path) -> bool:
    """True when the file exited 0 only because it skipped itself."""
    try:
        return TAP_SKIP_RE.search(log_path.read_text(encoding="utf-8", errors="replace")) is not None
    except OSError:
        return False


def discover(root: Path, corpus_dir: Path) -> list[str]:
    paths = [
        candidate.relative_to(root).as_posix()
        for candidate in sorted(corpus_dir.rglob("test-*"))
        if candidate.is_file() and candidate.suffix in {".js", ".mjs", ".cjs"}
    ]
    return paths


def read_file_list(root: Path, list_path: Path) -> list[str]:
    """Read a work-list of test paths (one per line, `#` comments allowed).

    A round targets one cluster, not the whole corpus: re-running all 4433
    files to score a 40-file fix costs ~30 minutes and buries the signal in
    unrelated flakiness. Paths may be repo-relative or absolute; both are
    normalised to the repo-relative form the TSV uses, so a cluster list and a
    full run stay directly comparable by corpus_diff.py.
    """
    selected: list[str] = []
    seen: set[str] = set()
    for raw in list_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        candidate = Path(line)
        relative = candidate.resolve().relative_to(root).as_posix() if candidate.is_absolute() else candidate.as_posix()
        if not (root / relative).is_file():
            raise SystemExit(f"--files entry does not exist: {line}")
        if relative not in seen:
            seen.add(relative)
            selected.append(relative)
    return selected


def write_outputs(output_dir: Path, results: list[Result]) -> None:
    columns = ["path", "exit_code", "classification", "duration_ms", "log"]
    with (output_dir / "results.tsv").open("w", encoding="utf-8") as stream:
        stream.write("\t".join(columns) + "\n")
        for result in results:
            values = asdict(result)
            stream.write("\t".join(str(values[column]) for column in columns) + "\n")
    categories: dict[str, int] = {}
    for result in results:
        categories[result.classification] = categories.get(result.classification, 0) + 1
    summary = {"files": len(results), "categories": dict(sorted(categories.items()))}
    (output_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path, help="mbun executable")
    parser.add_argument("--root", default=Path.cwd(), type=Path, help="repository root")
    parser.add_argument("--corpus", default=Path("compat/node/test/parallel"), type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--files",
        type=Path,
        help="run only the test paths listed in this file (one per line, '#' comments allowed) "
        "instead of discovering the whole corpus — use it to score one cluster",
    )
    parser.add_argument(
        "--filter",
        default="",
        help="keep only selected paths whose name contains this substring (e.g. test-fs-)",
    )
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    ensure_disk_headroom()
    root = args.root.resolve()
    binary = args.bin.resolve()
    output_dir = args.out.resolve()
    # Do NOT resolve the corpus dir: compat/node may be a symlink (worktree
    # setups point it at a sibling checkout). Resolving it would make the
    # discovered files fall outside `root`, breaking relative_to(root).
    corpus_dir = root / args.corpus
    paths = read_file_list(root, args.files) if args.files else discover(root, corpus_dir)
    if args.filter:
        paths = [path for path in paths if args.filter in path]
    if not paths:
        raise SystemExit("no test files selected")
    output_dir.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = [
            executor.submit(run_one, binary, root, output_dir, args.timeout, path, index % max(1, args.jobs))
            for index, path in enumerate(paths)
        ]
        results = [future.result() for future in futures]
    write_outputs(output_dir, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
