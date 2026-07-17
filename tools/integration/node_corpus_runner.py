#!/usr/bin/env python3
"""Run Node's test/parallel corpus directly through one mbun binary.

Node's upstream tests are plain scripts that expect exit code 0 (no test
runner): a file passes when mbun executes it to a clean exit. Harness
requirements node's own runner would provide (// Flags: comments, internal
bindings, python harness env) are NOT emulated — files needing them count as
failures, which keeps the measurement honest file-level coverage, comparable
across runs, rather than an API checklist.

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
import re
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from bounded_run import BoundedRun, ensure_disk_headroom


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


def run_one(binary: Path, root: Path, output_dir: Path, timeout: float, path: str) -> Result:
    started = time.monotonic()
    relative_log = Path("logs") / log_name(path)
    bounded = BoundedRun(
        output_dir / relative_log,
        private_tmp=output_dir / "tmp" / log_name(path)[:12],
    ).run([str(binary), str((root / path).resolve())], timeout=timeout, cwd=root)
    duration_ms = round((time.monotonic() - started) * 1000)
    if bounded.timed_out:
        classification = "timeout"
    elif bounded.oom_killed:
        classification = "oom-kill"
    elif bounded.exit_code == 0:
        classification = "pass"
    else:
        classification = "fail"
    return Result(path, bounded.exit_code, classification, duration_ms, str(relative_log))


def discover(root: Path, corpus_dir: Path) -> list[str]:
    paths = [
        candidate.relative_to(root).as_posix()
        for candidate in sorted(corpus_dir.rglob("test-*"))
        if candidate.is_file() and candidate.suffix in {".js", ".mjs", ".cjs"}
    ]
    return paths


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
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    ensure_disk_headroom()
    root = args.root.resolve()
    binary = args.bin.resolve()
    output_dir = args.out.resolve()
    corpus_dir = (root / args.corpus).resolve()
    paths = discover(root, corpus_dir)
    if not paths:
        raise SystemExit("no test files selected")
    output_dir.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = [
            executor.submit(run_one, binary, root, output_dir, args.timeout, path)
            for path in paths
        ]
        results = [future.result() for future in futures]
    write_outputs(output_dir, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
