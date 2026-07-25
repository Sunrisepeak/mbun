#!/usr/bin/env python3
"""failure_signature — turn a corpus run's logs into ranked, NAMED causes.

WHY THIS EXISTS: a corpus run reports pass/fail per file. Deciding what to work
on next needs the opposite view — which *mechanism* is behind the most files —
and every round so far rebuilt that view with a throwaway grep. Two costs, both
paid repeatedly:

  1. Every agent re-derived it. Measured: ~35 minutes each, and the result died
     with the agent. Wave 2 (agents triage their own subsystem) returned 3.7% of
     target; wave 3 (causes pre-named, no triage phase) returned 150%.
  2. A throwaway grep sees only the failure forms you thought to write down.
     A first pass over the bun corpus left 214 of 888 failures unexplained --
     a quarter of the surface invisible -- because it looked for
     `Expected:`/`Received:` and those files use `expect(x).toContain(y)`.

The output is deliberately NOT "top N log lines". Grouping by log text has
produced a wrong answer five separate times in this project: a shared message,
or a shared hang signature, is DOWNSTREAM of the cause. So this tool reports
what it extracted and how confident that extraction is, and it labels a group it
cannot explain as UNSPLIT rather than dressing it up as a mechanism.

Usage:
    failure_signature.py --run target/integration/r10-bun2 --corpus bun
    failure_signature.py --run target/integration/r10-real --corpus node --top 30
    failure_signature.py --run <dir> --corpus bun --cause "minifyTest"   # list files
    failure_signature.py --run <dir> --corpus node --json out.json

Exit codes: 0 always (this is an analysis tool, not a gate).
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# --- bun test output forms ---------------------------------------------------
# `expect(received).toContain("...")` then `Received: ...`. The MATCHER AND ITS
# ARGUMENT are the diagnostic part -- `toContain("util.ts:5:")` says "sourcemap
# did not remap" in a way that a bare value diff never does.
BUN_MATCHER_RE = re.compile(r"expect\((?:received|\w+)\)\.(\w+)\(([^\n]{0,120})\)")
BUN_RECEIVED_RE = re.compile(r"(?m)^\s*Received:\s*(.{0,160})")
BUN_EXPECTED_RE = re.compile(r"(?m)^\s*Expected:\s*(.{0,80})")
BUN_FAIL_NAME_RE = re.compile(r"(?m)^\s*\(fail\)\s*(.{0,90})")
# mbun's own "not implemented" errors are the most honest signal in the corpus:
# the runtime is telling you exactly what is missing.
DEFERRED_RE = re.compile(r"(?:not yet implemented in mbun|\(DEFERRED[^)]*\))[^\n]{0,90}")
DESTRUCTURE_RE = re.compile(r"Cannot destructure property '(\w+)'")

# --- node output forms -------------------------------------------------------
NODE_MUSTCALL_RE = re.compile(r"Mismatched (\S+) function calls\. Expected (\S+) (\d+), actual (\d+)")
NODE_MUSTNOTCALL_RE = re.compile(r"function should not have been called")
NODE_ASSERT_VALUES_RE = re.compile(r"strictly equal:.*?\n(.{0,200})", re.S)
NODE_CODE_RE = re.compile(r"\[(ERR_[A-Z0-9_]+)\]")
NODE_PROPERTY_CMP_RE = re.compile(r"Comparison of the '(\w+)' property failed: expected ('[^']+'|\S+)")
NODE_TYPED_THROW_RE = re.compile(r"Missing expected exception \((\w+)\)")

GENERIC_ERROR_RE = re.compile(r"(?m)^\s*(?:error:\s*)?((?:\w*Error|BuildMessage)[^\n]{0,120})")

# Anything with a digit run, a temp path, or a port is noise for grouping.
def normalise(text: str) -> str:
    text = re.sub(r"/(?:tmp|home)/\S+", "PATH", text)
    text = re.sub(r"\b\d+\b", "N", text)
    return " ".join(text.split())[:120]


@dataclass
class Signature:
    key: str
    detail: str
    confidence: str  # CAUSE | CLASS | MANIFESTATION | UNSPLIT


def bun_signature(output: str) -> Signature:
    """Order matters: the most specific, most explanatory form wins."""
    if (m := DESTRUCTURE_RE.search(output)) is not None:
        return Signature(f"missing internal-for-testing member: {m.group(1)}",
                         "a test destructured a namespace mbun does not populate",
                         "CAUSE")
    if (m := DEFERRED_RE.search(output)) is not None:
        return Signature("mbun declares this unimplemented", normalise(m.group(0)), "CAUSE")
    if "Failed to build service" in output and "failed to resolve" in output:
        return Signature("container registry unreachable",
                         "needs a pulled image; nothing about mbun is exercised", "CAUSE")
    if (m := BUN_MATCHER_RE.search(output)) is not None:
        matcher, arg = m.group(1), normalise(m.group(2))
        received = BUN_RECEIVED_RE.search(output)
        got = normalise(received.group(1)) if received else ""
        # A matcher argument is informative only when it is a LITERAL. The
        # argument is source text, so `expect(x).toBe(expected)` yields the bare
        # identifier "expected" -- a variable name, carrying nothing. Accepting
        # it put 223 files under `.toBe(expected)` and labelled them CLASS, i.e.
        # exactly the manifestation-as-mechanism error this tool exists to
        # prevent, committed by the tool itself on its first run.
        literal = bool(re.search(r"""["'`\[{]""", arg))
        informative = literal and len(arg) > 4
        return Signature(
            f".{matcher}({arg})" if informative else f".{matcher}(<bare value>)",
            f"received: {got}",
            "CLASS" if informative else "MANIFESTATION")
    if (m := GENERIC_ERROR_RE.search(output)) is not None:
        return Signature(normalise(m.group(1)), "", "CLASS")
    if (m := BUN_FAIL_NAME_RE.search(output)) is not None:
        return Signature(f"unparsed failure in: {normalise(m.group(1))}", "", "UNSPLIT")
    return Signature("no recognised failure form", "", "UNSPLIT")


def node_signature(output: str) -> Signature:
    if (m := NODE_MUSTCALL_RE.search(output)) is not None:
        return Signature("mustCall: an expected event never fired",
                         f"expected {m.group(2)} {m.group(3)}, actual {m.group(4)}", "CLASS")
    if NODE_MUSTNOTCALL_RE.search(output):
        return Signature("mustNotCall: an event fired that should not have",
                         "often a double-emit", "CLASS")
    if (m := DEFERRED_RE.search(output)) is not None:
        return Signature("mbun declares this unimplemented", normalise(m.group(0)), "CAUSE")
    # A node assertion that names a CONTRACT is a class; one that only says an
    # assertion failed is a manifestation. Distinguishing these is the whole
    # point of the confidence column, and getting it wrong the easy way reported
    # 84% of the node corpus as understood when the real figure is far lower:
    # "The expression evaluated to a falsy value" and a bare "AssertionError"
    # say nothing at all about a mechanism.
    if (m := NODE_PROPERTY_CMP_RE.search(output)) is not None:
        return Signature(f"wrong {m.group(1)}: expected {m.group(2)}", "", "CLASS")
    if (m := NODE_TYPED_THROW_RE.search(output)) is not None:
        return Signature(f"missing expected exception ({m.group(1)})", "", "CLASS")
    for empty in ("The expression evaluated to a falsy value",
                  "Missing expected exception.",
                  "Expected values to be strictly deep-equal:"):
        if empty in output:
            return Signature(f"bare assertion: {empty.rstrip(':.')}",
                             "names no contract -- split per file", "MANIFESTATION")
    if (m := NODE_CODE_RE.search(output)) is not None:
        code = m.group(1)
        if code == "ERR_ASSERTION":
            return Signature("bare assertion: ERR_ASSERTION",
                             "names no contract -- split per file", "MANIFESTATION")
        return Signature(f"coded error: {code}", "", "CLASS")
    if (m := NODE_ASSERT_VALUES_RE.search(output)) is not None:
        body = normalise(m.group(1))
        bare = re.fullmatch(r"[N\s!=+\-'\"truefals]*", body) is not None
        return Signature(f"value divergence: {body}", "",
                         "MANIFESTATION" if bare else "CLASS")
    if (m := GENERIC_ERROR_RE.search(output)) is not None:
        text = normalise(m.group(1))
        bare = re.fullmatch(r"AssertionError:?\s*", text + " ") is not None or text == "AssertionError"
        return Signature(text, "", "MANIFESTATION" if bare else "CLASS")
    return Signature("no recognised failure form", "", "UNSPLIT")


def load_rows(run: Path, corpus: str) -> list[tuple[str, str, str]]:
    """(path, classification, log-relative-path) for non-passing files."""
    results = run / "results.tsv"
    if not results.exists():
        raise SystemExit(f"no results.tsv under {run}")
    lines = results.read_text(encoding="utf-8", errors="replace").splitlines()
    header = lines[0].split("\t")
    cls_i = header.index("classification")
    log_i = header.index("log")
    green = {"green", "pass", "ahead-of-reference", "skipped", "all-skipped", "no-tests"}
    rows = []
    for line in lines[1:]:
        f = line.split("\t")
        if len(f) <= max(cls_i, log_i) or f[cls_i] in green:
            continue
        rows.append((f[0], f[cls_i], f[log_i]))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", required=True, type=Path, help="a corpus run output directory")
    ap.add_argument("--corpus", required=True, choices=("bun", "node"))
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--cause", help="print the files behind a cause (substring match)")
    ap.add_argument("--json", type=Path, help="write the full grouping")
    args = ap.parse_args()

    rows = load_rows(args.run, args.corpus)
    sign = bun_signature if args.corpus == "bun" else node_signature

    groups: dict[str, list[str]] = collections.defaultdict(list)
    confidence: dict[str, str] = {}
    details: dict[str, str] = {}
    unreadable = 0
    for path, _cls, log in rows:
        log_path = args.run / log
        if not log_path.exists():
            unreadable += 1
            continue
        sig = sign(log_path.read_text(encoding="utf-8", errors="replace"))
        groups[sig.key].append(path)
        confidence.setdefault(sig.key, sig.confidence)
        details.setdefault(sig.key, sig.detail)

    if args.cause:
        for key, files in groups.items():
            if args.cause.lower() in key.lower():
                print(f"# {key}  [{confidence[key]}]  {len(files)} files")
                for f in sorted(files):
                    print(f"  {f}")
        return 0

    ranked = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    named = sum(len(v) for k, v in groups.items() if confidence[k] in ("CAUSE", "CLASS"))
    print(f"{len(rows)} non-passing files, {len(groups)} signatures, "
          f"{named} attributed to a CAUSE or CLASS "
          f"({named * 100 // max(1, len(rows))}%)")
    if unreadable:
        print(f"  {unreadable} log(s) missing on disk")
    print()
    for key, files in ranked[:args.top]:
        mark = {"CAUSE": "  ", "CLASS": "  ", "MANIFESTATION": "! ", "UNSPLIT": "? "}[confidence[key]]
        print(f"{mark}{len(files):4}  [{confidence[key]:13}] {key}")
        if details[key]:
            print(f"        {details[key][:110]}")
    print("\n! = manifestation, not a mechanism — never budget it as one.")
    print("? = the extractor could not explain it; improving the extractor beats guessing.")

    if args.json:
        args.json.write_text(json.dumps(
            {k: {"confidence": confidence[k], "detail": details[k], "files": sorted(v)}
             for k, v in ranked}, indent=1, sort_keys=False) + "\n", encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
