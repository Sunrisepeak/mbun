#!/usr/bin/env python3
"""blocker_rank -- rank corpus files by how CLOSE they are, not by which area they are in.

    python3 tools/integration/blocker_rank.py --run target/integration/w63-fin-bun --corpus bun
    python3 tools/integration/blocker_rank.py --run target/integration/w64-fin-node --corpus node --top 30
    python3 tools/integration/blocker_rank.py --run <dir> --corpus bun --signature 'ERR_INVALID_ARG_TYPE'

WHY THIS EXISTS. Four consecutive waves picked targets by AREA -- "test-http2",
"cli/install", "js/web" -- and the returns collapsed: wave 64 spent ~17 lane-hours
for +2 attributable files, and two separate lanes landed real, correct, well-gated
fixes for exactly ZERO files. Both post-mortems said the same thing:

  wave 64 (ESM live bindings): the fix was right and the entry's "14 files" was
  wrong, because ESM was those files' FIRST blocker, not their only one. bun-add
  went from 0 expects evaluated to 143 -- and then died on install-CLI gaps.

  wave 65 (install CLI): cleared the argument gate those same files then hit.
  Result: 0 files. "Every remaining cli/install file has 2+ independent blockers,
  so clearing the argument gate only exposes the next one."

A file with three blockers pays nothing until all three are gone, and no area
ranking can see that. What decides whether work becomes a green file is the
number of blockers REMAINING in that file, and how many files share the one you
are about to fix. So rank on that instead.

WHAT IT MEASURES, and what it does not. It parses the per-file logs a corpus run
already wrote, extracts each distinct failure signature, and reports:

  - `blockers`  distinct signatures in a file -- a proxy for "how many separate
                causes must die before this file is green";
  - `sole`      for a signature: how many files have it as their ONLY blocker.
                That is the number of files a fix actually converts, and it is
                usually far smaller than the number of files that merely mention
                it. `sole` is the honest version of "this unblocks N files".

Signature extraction is a heuristic over log text, so `blockers` is a LOWER bound
on distinct causes (two failures can share one wording) and an UPPER bound on
independent ones (one cause can produce two wordings). It is a triage aid for
picking targets, not a proof. Verify a candidate by reading its log before
committing a lane to it.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

# Normalisations that turn one failure into a stable signature. Order matters:
# strip the volatile parts before the shape is compared.
_SUBS = (
    (re.compile(r'\b0x[0-9a-fA-F]+\b'), '0xH'),
    (re.compile(r'/[^\s:"\']+'), 'PATH'),          # absolute paths, incl. tmpdirs
    (re.compile(r'\b\d+(\.\d+)?(ms|s)\b'), 'DUR'), # durations
    (re.compile(r'\b\d+\b'), 'N'),                 # line numbers, counts, ports
    (re.compile(r'\s+'), ' '),
)

# Lines that carry a CAUSE. Deliberately excludes `(fail) <test name>` and
# `not ok <name>`: those name the test, not the reason, so grouping on them
# produced signatures like "test.js" and bare numbers -- 32 files "sharing" a
# blocker that was really just a common test name. A cause is an error class, an
# error code, or the assertion mismatch itself.
_CAUSE = re.compile(
    r'^\s*(?:'
    r'(?P<code>ERR_[A-Z0-9_]+|DEP\d{4})\b'
    r'|(?P<err>(?:[A-Za-z_$][\w$]*)?(?:Error|Exception)(?::.*)?)'
    r'|(?P<expect>expect\([^)]*\)\.[A-Za-z]+.*)'
    r'|(?P<mismatch>(?:Expected|Received)\s*:.*)'
    r'|(?P<assert>Assertion\b.*|AssertionError\b.*)'
    r'|(?P<wedged><run wedged>.*)'
    r')'
)


def signature(line: str) -> str | None:
    m = _CAUSE.match(line)
    if not m:
        return None
    s = next((v for v in m.groupdict().values() if v), None)
    if not s:
        return None
    for pat, rep in _SUBS:
        s = pat.sub(rep, s)
    s = s.strip()[:160]
    return s or None


def load(run: Path, corpus: str) -> tuple[dict[str, str], str]:
    tsv = run / "results.tsv"
    if not tsv.exists():
        tsv = run / "results.partial.tsv"
    if not tsv.exists():
        raise SystemExit(f"blocker_rank: no results.tsv in {run}")
    green = "green" if corpus == "bun" else "pass"
    rows: dict[str, str] = {}
    with tsv.open() as f:
        r = csv.reader(f, delimiter='\t')
        hdr = next(r)
        ci, li = hdr.index("classification"), (hdr.index("log") if "log" in hdr else -1)
        for row in r:
            if len(row) <= ci:
                continue
            if row[ci] == green:
                continue
            log = row[li] if li >= 0 and len(row) > li else ""
            rows[row[0]] = log
    return rows, green


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", type=Path, required=True, help="a corpus run dir with logs/")
    ap.add_argument("--corpus", choices=("node", "bun"), required=True)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--signature", help="list the files whose ONLY blocker is this signature")
    ap.add_argument("--max-blockers", type=int, default=1,
                    help="when listing near-green files, the blocker count to show (default 1)")
    ap.add_argument("--list-near", action="store_true",
                    help="print the near-green files themselves, with their single blocker")
    args = ap.parse_args()

    rows, _ = load(args.run, args.corpus)
    per_file: dict[str, set[str]] = {}
    for path, log in rows.items():
        sigs: set[str] = set()
        lp = args.run / log if log else None
        if lp and lp.exists():
            try:
                for line in lp.read_text(errors="replace").splitlines():
                    s = signature(line)
                    if s:
                        sigs.add(s)
            except OSError:
                pass
        per_file[path] = sigs

    scored = {p: s for p, s in per_file.items() if s}
    unparsed = len(per_file) - len(scored)

    sole: dict[str, list[str]] = defaultdict(list)
    for p, s in scored.items():
        if len(s) == 1:
            sole[next(iter(s))].append(p)

    if args.signature:
        hits = sole.get(args.signature, [])
        print(f"files whose ONLY blocker is {args.signature!r}: {len(hits)}")
        for p in sorted(hits):
            print("  " + p)
        return 0

    print(f"=== blocker rank: {args.run.name} ({args.corpus}) ===")
    print(f"  {len(per_file)} non-green files, {len(scored)} with a parsed signature "
          f"({unparsed} unparsed -- timeouts/oom/crashes usually leave no cause line)\n")

    near = sorted((p for p, s in scored.items() if len(s) <= args.max_blockers),
                  key=lambda p: (len(scored[p]), p))
    print(f"  {len(near)} file(s) at <= {args.max_blockers} blocker(s) -- these are what a fix can convert")
    if args.list_near:
        for p in near:
            print(f"    [{len(scored[p])}] {p}  <- {next(iter(scored[p]))[:88]}")

    print(f"\n  top signatures by SOLE-blocker count (files a fix actually converts):")
    ranked = sorted(sole.items(), key=lambda kv: -len(kv[1]))[:args.top]
    if not ranked:
        print("    (none -- every non-green file has more than one distinct signature)")
    for sig, files in ranked:
        mentions = sum(1 for s in scored.values() if sig in s)
        print(f"    {len(files):4d} sole  ({mentions:4d} mention)  {sig[:96]}")

    print("\n  CAVEAT: signatures are heuristic over log text. `sole` is a triage aid,")
    print("  not a proof -- read the logs of a candidate before committing a lane to it.")
    print("  In particular a bare assertion WRAPPER (\"AssertionError: Expected values to be")
    print("  strictly equal\") is a shared assertion FORM, not a shared cause: 65 files ranking")
    print("  under it do not become green from one fix. Rank on it to find near-green FILES,")
    print("  not to size a fix. The file list (--list-near) is the trustworthy output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
