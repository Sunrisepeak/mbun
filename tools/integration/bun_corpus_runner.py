#!/usr/bin/env python3
"""Run a deterministic slice of vendored bun tests through one mbun binary."""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import hashlib
import json
import os
import re
import time
from dataclasses import asdict, dataclass
from pathlib import Path


PASS_RE = re.compile(r"(?m)^\s*(\d+) pass\s*$")
FAIL_RE = re.compile(r"(?m)^\s*(\d+) fail\s*$")
SKIP_RE = re.compile(r"(?m)^\s*(\d+) skip\s*$")
# bun's corpus marks cases bun itself gets wrong with `test.failing`. When mbun
# is MORE correct than bun, that marker passes, and bun's runner reports it as
# a failure: "(fail) <name> — expected to fail but passed".
#
# Scoring that as `test-failure` is a measurement bug with real consequences: it
# makes advancing node compatibility look like a bun-compatibility REGRESSION,
# which is exactly backwards, and it manufactures a "which corpus do we serve?"
# trade-off that does not exist. The deep-equality work hit this — bun's
# deep-equal suite states outright that its expectations are node's semantics
# and that deviations are "cases Bun gets wrong today", yet mbun had implemented
# bun's bugs on purpose because the corpus scored them as required behaviour.
#
# Being ahead of the reference implementation is its own outcome, not a failure.
# A stale `test.failing` marker. mbun used to print its own wording ("— expected
# to fail but passed"); it now prints bun's ("this test is marked as failing but
# it passed…"), so both spellings have to be recognised or the ahead-of-reference
# bucket silently empties out.
AHEAD_RE = re.compile(r"expected to fail but passed|marked as failing but it passed")
EXPECT_RE = re.compile(r"(?m)^\s*(\d+) expect\(\) calls\s*$")
RAN_RE = re.compile(r"Ran (\d+) tests?")
TEST_FILE_RE = re.compile(r"\.test\.(?:[cm]?[jt]sx?)$")
# An error the runner reported outside any test: an unhandled rejection, a
# missing global, a module that threw while loading. Distinguishes "the file
# loaded and simply declares no tests" from "the file died before declaring any".
ERROR_MARK_RE = re.compile(r"(?m)^\s*\d+ error\s*$|^\s*error:|^# Unhandled error")
ERROR_COUNT_RE = re.compile(r"(?m)^\s*(\d+) error\s*$")
# The crash handler (modules/crash_handler) prints this banner on a fatal
# signal. Before it was wired up, a crashing file was scored by whatever partial
# output survived (usually test-failure, sometimes load-error) — now the crash
# is visible and gets its own bucket, so a native fault is never miscounted as a
# clean test failure or a load error.
CRASH_RE = re.compile(r"^=== mbun crashed: ", re.M)


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


DEFAULT_BLOCKED_MANIFEST = Path(__file__).resolve().parent / "manifests" / "blocked-external.txt"


def load_patterns(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return read_list(path)


def classify(
    exit_code: int,
    passed: int,
    failed: int,
    ran: int,
    skipped: int,
    timed_out: bool,
    oom_killed: bool,
    output: str,
    blocked: bool = False,
) -> str:
    if timed_out:
        # A file that needs MySQL/Redis/the npm registry expresses that as a
        # connect that never completes, so it lands in `timeout` and buries the
        # files where mbun itself hangs -- 51 of 107 in the 2026-07-20 round.
        # The manifest is triaged by hand and only ever redirects a timeout;
        # `blocked-external` is not a pass and verifies nothing (see #4).
        return "blocked-external" if blocked else "timeout"
    if oom_killed:
        return "oom-kill"
    # A native fault (SIGSEGV/SIGABRT/…) is its own outcome: the crash handler
    # printed a backtrace, so this is a real mbun bug, not a test that merely
    # failed an assertion. Checked before the pass/fail branch because a file can
    # crash *after* printing some results (the napi suites run a test, then abort
    # during env teardown).
    if CRASH_RE.search(output):
        return "crash"
    if ran > 0 or passed > 0 or failed > 0:
        # An out-of-test error (a rejected describe body, an unhandled rejection
        # between tests) is a failed file for bun, which exits non-zero on it;
        # mbun currently still exits 0, so exit code alone would score such a
        # file as a full green even though a whole scope may have been dropped.
        # Count the failures that are only "we are more correct than bun".
        ahead = len(AHEAD_RE.findall(output))
        real_failed = max(0, failed - ahead)
        errors = last_int(ERROR_COUNT_RE, output)
        # A file whose ONLY failures are stale `test.failing` markers is ahead of
        # the reference, and the non-zero exit is the expected consequence of that
        # -- bun's runner exits 1 precisely BECAUSE a failing-marked test passed.
        # Testing `exit_code != 0` before this check made the ahead-of-reference
        # bucket unreachable for the exact case it was added for: measured on the
        # full corpus, assert/deep-equal.test.ts had 22 of 22 failures be
        # "expected to fail but passed" and was still scored test-failure. Only a
        # non-zero exit is forgiven, and only when nothing else went wrong.
        if ahead > 0 and real_failed == 0 and errors == 0:
            return "ahead-of-reference"
        if exit_code != 0 or real_failed > 0 or errors > 0:
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
    # "Loaded fine, declares no tests" is not a load error. Six corpus files are
    # like this by design -- empty-file.test.ts is a comment-only regression
    # guard, harness.test.js and svelte/server-side.test.ts are fully commented
    # out, expect-type-doctest.test.ts only makes compile-time type assertions,
    # issue-2086.test.ts guards its tests behind `typeof setImmediate ===
    # "undefined"`, and net/handle-leak.test.ts is a top-level script with no
    # test() blocks. Reporting them as load-error hid the files that genuinely
    # fail to load. Requires a clean exit AND a runner summary AND no
    # out-of-test error, so a file that dies before registering anything (an
    # unhandled error, a missing global) still lands in load-error.
    if exit_code == 0 and RAN_RE.search(output) and not ERROR_MARK_RE.search(output):
        return "no-tests"
    return "load-error"


def log_name(path: str) -> str:
    digest = hashlib.sha1(path.encode("utf-8"), usedforsecurity=False).hexdigest()[:12]
    readable = re.sub(r"[^A-Za-z0-9_.-]+", "_", path)[-96:]
    return f"{digest}-{readable}.log"


from bounded_run import (
    DEFAULT_MEMORY_MAX,
    DEFAULT_TASKS_MAX,
    BoundedRun,
    ensure_disk_headroom,
)


def run_one(
    binary: Path, root: Path, output_dir: Path, timeout: float, path: str, spawn_cwd: Path,
    blocked_patterns: list[str] | None = None,
    memory_max: str = DEFAULT_MEMORY_MAX,
    tasks_max: int = DEFAULT_TASKS_MAX,
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
        memory_max=memory_max,
        tasks_max=tasks_max,
    )
    output = runner.read_output()
    duration_ms = round((time.monotonic() - started) * 1000)
    passed = last_int(PASS_RE, output)
    failed = last_int(FAIL_RE, output)
    expects = last_int(EXPECT_RE, output)
    ran = last_int(RAN_RE, output)
    skipped = last_int(SKIP_RE, output)
    blocked = any(fnmatch.fnmatch(path, pattern) for pattern in (blocked_patterns or []))
    category = classify(bounded.exit_code, passed, failed, ran, skipped,
                        bounded.timed_out, bounded.oom_killed, output, blocked)
    return Result(path, bounded.exit_code, passed, failed, expects, ran, category,
                  duration_ms, str(relative_log))


def ensure_corpus_dependencies(corpus_root: Path) -> None:
    """Refuse to measure a corpus whose npm dependencies are not installed.

    `node_modules/` is gitignored inside the bun submodule, so a fresh clone
    starts empty -- and 264 of the 1902 files then die at file evaluation with
    "Cannot find module" before a single assertion runs (esbuild alone accounts
    for 76: test/bundler/expectBundled.ts imports it at the top of the shared
    bundler harness). Those files look like one-assertion near-misses in the
    results while actually being dead, which silently misdirects a whole round
    of triage. Fail loudly instead.
    """
    missing = [
        directory
        for directory in (corpus_root, corpus_root / "test")
        if not (directory / "node_modules").is_dir()
    ]
    if missing:
        listed = "\n".join(f"  (cd {directory} && bun install --frozen-lockfile)"
                            for directory in missing)
        raise SystemExit(
            "bun_corpus_runner: corpus dependencies are not installed, so hundreds "
            "of files would fail as 'Cannot find module' instead of being "
            f"measured. Install them first:\n{listed}\n"
            "(pass --allow-missing-node-modules to measure the un-provisioned "
            "state on purpose)"
        )


def read_list(path: Path) -> list[str]:
    """A newline-separated list of test paths -- and NOT a results.tsv.

    Handing this a results.tsv is an easy mistake (both are line-oriented, both
    start with a path) and it used to fail silently: every row's trailing
    tab-separated columns became part of the "path", nothing matched, and the run
    reported ~1900 `no-tests` rows. That reads exactly like a catastrophic
    regression rather than like a bad argument, and a lane lost a full round to
    it. So detect the shape and refuse.
    """
    values: list[str] = []
    tabbed = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if not value or value.startswith("#"):
            continue
        if "\t" in value:
            tabbed += 1
        values.append(value)
    if tabbed:
        raise SystemExit(
            f"bun_corpus_runner: --list {path} looks like a TSV, not a path list "
            f"({tabbed} of {len(values)} lines contain tabs).\n"
            f"  A results.tsv passed here yields ~1900 silent 'no-tests' rows that "
            f"look like a total regression.\n"
            f"  Extract the paths first:  awk -F'\\t' 'NR>1{{print $1}}' {path} > list.txt")
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
        # Installed packages ship their own tests: once the corpus npm
        # dependencies exist, test/node_modules adds 785 third-party .test.*
        # files, inflating the corpus from 1902 to 2687 and mixing other
        # projects' suites into mbun's compatibility numbers.
        if "node_modules" in relative_corpus.parts:
            continue
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


# --- crash-resilient journal -------------------------------------------------
# write_outputs() runs once, at the very end. A full bun corpus pass is ~2 hours,
# and a background run of that length in this environment gets killed before it
# finishes -- which used to throw away every measured file. The node runner grew
# the same journal for the same reason after a SIGTERM lost a 4433-file run; a
# later kill kept 3960 of them.
#
# One line per finished file, flushed AND fsynced, so a kill at any instant loses
# at most the file in flight. --resume reads it back and skips what is already
# measured.
JOURNAL_NAME = "results.partial.tsv"
JOURNAL_COLUMNS = ["path", "exit_code", "passed", "failed", "expects", "ran",
                   "classification", "duration_ms", "log"]


def journal_append(stream, result: Result) -> None:
    values = asdict(result)
    stream.write("\t".join(str(values[column]) for column in JOURNAL_COLUMNS) + "\n")
    stream.flush()
    os.fsync(stream.fileno())


def journal_load(output_dir: Path) -> dict[str, Result]:
    """Previously measured results, keyed by path. A truncated final line (the
    file that was in flight when the kill landed) is discarded, not guessed at."""
    journal = output_dir / JOURNAL_NAME
    if not journal.exists():
        return {}
    done: dict[str, Result] = {}
    for line in journal.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = line.split("\t")
        if len(fields) != len(JOURNAL_COLUMNS):
            continue
        try:
            done[fields[0]] = Result(
                path=fields[0], exit_code=int(fields[1]), passed=int(fields[2]),
                failed=int(fields[3]), expects=int(fields[4]), ran=int(fields[5]),
                classification=fields[6], duration_ms=int(fields[7]), log=fields[8])
        except ValueError:
            continue
    return done


def write_outputs(
    output_dir: Path,
    results: list[Result],
    memory_max: str = DEFAULT_MEMORY_MAX,
    tasks_max: int = DEFAULT_TASKS_MAX,
) -> None:
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
        "resource_profile": {"memory_max": memory_max, "tasks_max": tasks_max},
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
    parser.add_argument(
        "--memory-max", default=DEFAULT_MEMORY_MAX,
        help="systemd MemoryMax for each file scope (default: %(default)s); "
             "a non-default profile requires --jobs 1",
    )
    parser.add_argument(
        "--tasks-max", type=int, default=DEFAULT_TASKS_MAX,
        help="systemd TasksMax for each file scope (default: %(default)s); "
             "a non-default profile requires --jobs 1",
    )
    parser.add_argument(
        "--resume", action="store_true",
        help="skip files already recorded in results.partial.tsv and append to it, "
             "so a killed run continues instead of starting over",
    )
    parser.add_argument(
        "--allow-missing-node-modules", action="store_true",
        help="measure even when the corpus npm dependencies are absent",
    )
    parser.add_argument(
        "--blocked-manifest", type=Path, default=DEFAULT_BLOCKED_MANIFEST,
        help="paths that time out only because a service/registry/toolchain is "
             "absent; they are reported as blocked-external instead of timeout",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ensure_disk_headroom()
    if args.jobs < 1:
        raise SystemExit("--jobs must be positive")
    if args.tasks_max < 1:
        raise SystemExit("--tasks-max must be positive")
    if (args.memory_max != DEFAULT_MEMORY_MAX or args.tasks_max != DEFAULT_TASKS_MAX) and args.jobs != 1:
        raise SystemExit("an overridden resource profile requires --jobs 1")
    root = args.root.resolve()
    binary = args.bin.resolve()
    output_dir = args.out.resolve()
    spawn_cwd = args.cwd.resolve() if args.cwd else root
    if args.list:
        paths = read_list(args.list)
    else:
        if args.sample_per_group < 1:
            raise SystemExit("--sample-per-group must be positive")
        # Keep the corpus path lexical during discovery. Worktrees commonly
        # symlink compat/bun to one shared read-only checkout; resolving that
        # symlink makes candidate.relative_to(root) fail even though the
        # user-visible corpus path is still rooted at --root. run_one resolves
        # the selected file only when spawning it.
        discover_root = args.discover if args.discover.is_absolute() else root / args.discover
        paths = discover(root, discover_root, args.sample_per_group)
    if args.max_files is not None:
        if args.max_files < 1:
            raise SystemExit("--max-files must be positive")
        paths = sorted(
            paths,
            key=lambda value: (hashlib.sha256(value.encode("utf-8")).digest(), value),
        )[: args.max_files]
    if not paths:
        raise SystemExit("no test files selected")

    if not args.allow_missing_node_modules:
        ensure_corpus_dependencies((args.cwd or root).resolve())
    blocked_patterns = load_patterns(args.blocked_manifest.resolve())
    output_dir.mkdir(parents=True, exist_ok=True)
    done = journal_load(output_dir) if args.resume else {}
    pending = [path for path in paths if str(path) not in done]
    if done:
        print(f"resuming: {len(done)} already measured, {len(pending)} to go", flush=True)
    # Append on --resume, truncate otherwise, so a fresh run never inherits a
    # stale journal from a previous binary.
    results = list(done.values())
    with (output_dir / JOURNAL_NAME).open("a" if args.resume else "w", encoding="utf-8") as journal:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
            futures = [
                executor.submit(run_one, binary, root, output_dir, args.timeout, path, spawn_cwd,
                                blocked_patterns, args.memory_max, args.tasks_max)
                for path in pending
            ]
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                journal_append(journal, result)
                results.append(result)
    # Stable order regardless of completion order, so two runs diff cleanly.
    results.sort(key=lambda result: result.path)
    write_outputs(output_dir, results, args.memory_max, args.tasks_max)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
