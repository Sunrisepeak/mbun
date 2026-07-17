#!/usr/bin/env python3
"""Run a deterministic slice of vendored bun tests through one mbun binary."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import re
import time
from dataclasses import asdict, dataclass
from pathlib import Path


PASS_RE = re.compile(r"(?m)^\s*(\d+) pass\s*$")
FAIL_RE = re.compile(r"(?m)^\s*(\d+) fail\s*$")
SKIP_RE = re.compile(r"(?m)^\s*(\d+) skip\s*$")
EXPECT_RE = re.compile(r"(?m)^\s*(\d+) expect\(\) calls\s*$")
RAN_RE = re.compile(r"Ran (\d+) tests?")
TEST_FILE_RE = re.compile(r"\.test\.(?:[cm]?[jt]sx?)$")


@dataclass(frozen=True)
class Result:
    path: str
    exit_code: int
    passed: int
    failed: int
    expects: int
    ran: int
    classification: str
    duration_ms: int
    log: str


def last_int(pattern: re.Pattern[str], output: str) -> int:
    matches = pattern.findall(output)
    return int(matches[-1]) if matches else 0


def classify(
    exit_code: int,
    passed: int,
    failed: int,
    ran: int,
    skipped: int,
    timed_out: bool,
    oom_killed: bool,
    output: str,
) -> str:
    if timed_out:
        return "timeout"
    if oom_killed:
        return "oom-kill"
    if ran > 0 or passed > 0 or failed > 0:
        if exit_code != 0 or failed > 0:
            return "test-failure"
        # A file whose every test was skipped exits 0 with 0 failures and so used
        # to score as a full green -- ci-restrictions.test.ts reported 0 pass /
        # 12 skip / 0 fail and counted as one. 25 corpus files gate on Bun.version,
        # which mbun reports as 0.1.0-mbun, so a whole suite silently vanishing is
        # not hypothetical. Skipped-only files are their own bucket: not a failure,
        # but nothing was verified either.
        return "green" if passed > 0 else "all-skipped"
    if "Cannot find module" in output and "cannot find package" in output:
        return "missing-dependency"
    if "Docker Compose file not found" in output:
        return "missing-fixture"
    if re.search(r"node-gyp build .* failed", output):
        return "fixture-build-error"
    return "load-error"


def log_name(path: str) -> str:
    digest = hashlib.sha1(path.encode("utf-8"), usedforsecurity=False).hexdigest()[:12]
    readable = re.sub(r"[^A-Za-z0-9_.-]+", "_", path)[-96:]
    return f"{digest}-{readable}.log"


from bounded_run import BoundedRun, ensure_disk_headroom


def run_one(
    binary: Path, root: Path, output_dir: Path, timeout: float, path: str, spawn_cwd: Path
) -> Result:
    started = time.monotonic()
    # All resource/safety bounding (systemd scope limits, own session, private
    # TMPDIR, file-backed output) lives in bounded_run.BoundedRun — the shared
    # protection layer for every harness on this machine.
    # `path` stays root-relative (it is the stable result identity across rounds);
    # the spawned command gets an absolute path so spawn_cwd can differ from root.
    relative_log = Path("logs") / log_name(path)
    runner = BoundedRun(
        output_dir / relative_log,
        private_tmp=output_dir / "tmp" / log_name(path)[:12],
    )
    bounded = runner.run(
        [str(binary), "test", str((root / path).resolve())],
        timeout=timeout,
        cwd=spawn_cwd,
    )
    output = runner.read_output()
    duration_ms = round((time.monotonic() - started) * 1000)
    passed = last_int(PASS_RE, output)
    failed = last_int(FAIL_RE, output)
    expects = last_int(EXPECT_RE, output)
    ran = last_int(RAN_RE, output)
    skipped = last_int(SKIP_RE, output)
    category = classify(bounded.exit_code, passed, failed, ran, skipped,
                        bounded.timed_out, bounded.oom_killed, output)
    return Result(path, bounded.exit_code, passed, failed, expects, ran, category,
                  duration_ms, str(relative_log))


def read_list(path: Path) -> list[str]:
    values: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if value and not value.startswith("#"):
            values.append(value)
    return values


def group_key(relative: Path) -> str:
    directories = relative.parent.parts
    return "/".join(directories[:3])


def discover(root: Path, corpus_root: Path, per_group: int) -> list[str]:
    groups: dict[str, list[str]] = {}
    for candidate in corpus_root.rglob("*"):
        if not candidate.is_file():
            continue
        relative_corpus = candidate.relative_to(corpus_root)
        if not TEST_FILE_RE.search(relative_corpus.as_posix()):
            continue
        relative_root = candidate.relative_to(root).as_posix()
        groups.setdefault(group_key(relative_corpus), []).append(relative_root)

    selected: list[str] = []
    for key in sorted(groups):
        ranked = sorted(
            groups[key],
            key=lambda value: (hashlib.sha256(value.encode("utf-8")).digest(), value),
        )
        selected.extend(ranked[:per_group])
    return selected


def write_outputs(output_dir: Path, results: list[Result]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "selected-tests.txt").write_text(
        "".join(f"{result.path}\n" for result in results), encoding="utf-8"
    )
    columns = ["path", "exit_code", "passed", "failed", "expects", "ran", "classification", "duration_ms", "log"]
    with (output_dir / "results.tsv").open("w", encoding="utf-8") as stream:
        stream.write("\t".join(columns) + "\n")
        for result in results:
            values = asdict(result)
            stream.write("\t".join(str(values[column]) for column in columns) + "\n")

    categories: dict[str, int] = {}
    for result in results:
        categories[result.classification] = categories.get(result.classification, 0) + 1
    summary = {
        "files": len(results),
        "passed": sum(result.passed for result in results),
        "failed": sum(result.failed for result in results),
        "ran": sum(result.ran for result in results),
        "expects": sum(result.expects for result in results),
        "categories": dict(sorted(categories.items())),
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path, help="mbun executable")
    parser.add_argument("--root", default=Path.cwd(), type=Path, help="repository root")
    parser.add_argument(
        "--cwd",
        type=Path,
        help="working directory to spawn each test from (default: --root). "
        "bun's own CI runs `bun test` from the bun repo root, so the vendored "
        "corpus wants --cwd <root>/.mbun/bun-ref: tests that spawn relative "
        "fixture paths (test/...) false-fail from anywhere else, and the "
        "corpus bunfig.toml is only picked up there.",
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--list", type=Path, help="newline-separated test paths")
    source.add_argument("--discover", type=Path, help="vendored compat/bun/test directory")
    parser.add_argument("--sample-per-group", type=int, default=1)
    parser.add_argument("--max-files", type=int, help="stable cap after stratified discovery")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=30.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ensure_disk_headroom()
    root = args.root.resolve()
    binary = args.bin.resolve()
    output_dir = args.out.resolve()
    spawn_cwd = args.cwd.resolve() if args.cwd else root
    if args.list:
        paths = read_list(args.list)
    else:
        if args.sample_per_group < 1:
            raise SystemExit("--sample-per-group must be positive")
        paths = discover(root, args.discover.resolve(), args.sample_per_group)
    if args.max_files is not None:
        if args.max_files < 1:
            raise SystemExit("--max-files must be positive")
        paths = sorted(
            paths,
            key=lambda value: (hashlib.sha256(value.encode("utf-8")).digest(), value),
        )[: args.max_files]
    if not paths:
        raise SystemExit("no test files selected")

    output_dir.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        futures = [
            executor.submit(run_one, binary, root, output_dir, args.timeout, path, spawn_cwd)
            for path in paths
        ]
        results = [future.result() for future in futures]
    write_outputs(output_dir, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
