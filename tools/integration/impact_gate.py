#!/usr/bin/env python3
"""impact_gate -- derive the corpus files a change can actually reach.

Replaces the full-corpus run as the default regression gate. Given a diff, it
extracts the observable identifiers the change touches and greps the corpus for
files that mention any of them, producing a `--files` list for the runners.

    # what does my working tree reach?
    python3 tools/integration/impact_gate.py --out /tmp/impact.txt

    # a specific range, both corpora, with the reasons
    python3 tools/integration/impact_gate.py --rev-range HEAD~3..HEAD --explain

    # then gate on exactly that set
    python3 tools/integration/node_corpus_runner.py --bin auto --files /tmp/impact.txt ...

WHY THIS EXISTS. The campaign kept reaching for a full 4433 + 1902 run whenever a
change touched something shared, and measured, that was over half of one session's
entire measurement budget -- 19,084 file-executions against a 6,335-file corpus,
i.e. the whole corpus re-run three times, mostly to re-confirm things already known
per-file. It is also actively worse as a gate than a targeted set, because running
1000+ files concurrently is what produces the load noise that fakes regressions
(measured: 60 files passing idle and failing under load on the SAME binary).

Every targeted gate built by hand this way did its job: 311 files for a change to
`assert`'s RegExp brand check, 105 for `process.config`, 36 for `process.versions`.
Two of them caught real regressions a subtree gate had missed; the rest proved
cleanliness in minutes rather than an hour.

WHAT IT DOES NOT DO. It cannot see a change that is observable only through
behaviour with no name in it -- an ordering change, a timing change, a GC change.
For those, name the surface yourself with --extra-symbol, or fall back to a full
run and say why. The tool prints this caveat with every result rather than letting
a clean report imply more than it proved.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

NODE_CORPUS = Path("compat/node/test/parallel")
BUN_CORPUS = Path("compat/bun/test")

# Identifiers that appear in almost every test and would select the whole corpus.
# Grepping for these is the same as not grepping at all, so they are refused.
TOO_COMMON = {
    "require", "module", "exports", "default", "value", "name", "length", "type",
    "data", "error", "err", "code", "message", "options", "opts", "result", "res",
    "req", "cb", "callback", "fn", "self", "this", "get", "set", "on", "off",
    "emit", "close", "end", "write", "read", "start", "stop", "run", "test", "it",
    "expect", "assert", "console", "log", "process", "buffer", "string", "number",
    "object", "array", "promise", "then", "catch", "async", "await", "return",
    "const", "let", "var", "function", "class", "new", "true", "false", "null",
    "undefined", "if", "else", "for", "while", "try", "throw", "typeof", "in", "of",
}

# What a changed line can plausibly expose to a test, in rough order of specificity.
PATTERNS = (
    # `M["node:sqlite"]`, `require("node:x")`, "bun:sqlite"
    re.compile(r'["\'](?:node:|bun:|internal/)[a-zA-Z0-9_/.\-]+["\']'),
    # ERR_FOO_BAR / DEP0123 / SQLITE_OK -- error codes are the strongest signal
    re.compile(r'\b(?:ERR_[A-Z0-9_]+|DEP\d{4}|[A-Z][A-Z0-9]*_[A-Z0-9_]{3,})\b'),
    # class Foo / function fooBar / fooBar( / .fooBar =
    re.compile(r'\bclass\s+([A-Z][A-Za-z0-9_]{2,})'),
    re.compile(r'\bfunction\s+([a-zA-Z_][A-Za-z0-9_]{3,})'),
    # `.fooBar =` is a DEFINITION; `.fooBar(` is a call. Matching calls swept in
    # `open`, `query`, `iterate`, `isArray` from the implementation's own internals
    # and unioned to 1089 node files -- as unselective as running everything.
    re.compile(r'\.([a-zA-Z_][A-Za-z0-9_]{3,})\s*='),
    # quoted property keys, e.g. "v8_enable_i18n_support"
    re.compile(r'["\']([a-z][a-z0-9_]{6,})["\']'),
)


def changed_lines(rev_range: str | None) -> list[str]:
    cmd = ["git", "diff", "--unified=0"]
    if rev_range:
        cmd.append(rev_range)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    lines = [l[1:] for l in proc.stdout.splitlines()
             if l.startswith("+") and not l.startswith("+++")]
    if not lines and not rev_range:
        # nothing staged/unstaged: fall back to the last commit, which is what a
        # caller almost always means right after committing.
        proc = subprocess.run(["git", "diff", "--unified=0", "HEAD~1..HEAD"],
                              cwd=ROOT, capture_output=True, text=True)
        lines = [l[1:] for l in proc.stdout.splitlines()
                 if l.startswith("+") and not l.startswith("+++")]
    return lines


def symbols(lines: list[str], extra: list[str]) -> tuple[set[str], set[str]]:
    """Return (usable symbols, symbols refused for being too common)."""
    found: set[str] = set()
    for line in lines:
        # A comment-only addition exposes nothing. This matters: several commits in
        # this campaign were 90% explanatory comments, and treating their prose as
        # API would have selected most of the corpus.
        stripped = line.strip()
        if stripped.startswith(("//", "#", "*", "/*")):
            continue
        for pat in PATTERNS:
            for m in pat.finditer(line):
                found.add((m.group(1) if m.groups() else m.group(0)).strip("\"'"))
    found.update(extra)
    refused = {s for s in found if s.lower() in TOO_COMMON or len(s) < 4}
    return found - refused, refused


def corpus_files(corpus: Path, run_dir: Path | None) -> list[Path]:
    """The files the RUNNER would execute -- not everything on disk.

    A naive rglob over compat/bun/test returned 8556 candidates for a 1902-file
    corpus, because it swept fixtures, node_modules and helper trees. Prefer the
    authoritative list from a real run's results.tsv; fall back to a shallow glob.
    """
    if run_dir:
        tsv = run_dir / "results.tsv"
        if not tsv.exists():
            tsv = run_dir / "results.partial.tsv"
        if tsv.exists():
            out = []
            for line in tsv.read_text().splitlines()[1:]:
                path = line.split("\t")[0].strip()
                if path.startswith(corpus.as_posix()):
                    out.append(ROOT / path)
            if out:
                return out
    base = ROOT / corpus
    if not base.is_dir():
        return []
    out = []
    for ext in ("*.js", "*.mjs", "*.cjs", "*.ts", "*.tsx"):
        out.extend(base.glob(ext))
    return out


def select(syms: set[str], corpus: Path, explain: bool, run_dir: Path | None,
           spread: float) -> tuple[dict[str, set[str]], dict[str, int]]:
    """file -> matching symbols, plus the per-symbol frequency that was used.

    A symbol present in most of the corpus does not discriminate -- gating on it is
    the same as running everything. `ERR_INVALID_ARG_TYPE` alone selected 1089 node
    files, and `open`/`query`/`iterate` were little better. So symbol frequency is
    measured first and anything above --spread of the corpus is dropped, with the
    number reported rather than silently applied.
    """
    files = corpus_files(corpus, run_dir)
    hits: dict[str, set[str]] = {}
    if not syms or not files:
        return hits, {}

    texts: list[tuple[str, str]] = []
    for f in files:
        try:
            texts.append((f.relative_to(ROOT).as_posix(), f.read_text(errors="replace")))
        except (OSError, ValueError):
            continue

    freq = {s: sum(1 for _, txt in texts if s in txt) for s in syms}
    limit = max(1, int(len(texts) * spread))
    keep = {s for s, n in freq.items() if 0 < n <= limit}

    for rel, txt in texts:
        matched = {s for s in keep if s in txt}
        if matched:
            hits[rel] = matched if explain else set()
    return hits, freq


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rev-range", help="e.g. HEAD~3..HEAD; default is the working tree")
    ap.add_argument("--out", type=Path, help="write the node file list here")
    ap.add_argument("--bun-out", type=Path, help="write the bun file list here")
    ap.add_argument("--extra-symbol", action="append", default=[],
                    help="name a surface the diff does not spell out; repeatable")
    ap.add_argument("--explain", action="store_true", help="show which symbol selected each file")
    ap.add_argument("--max-symbols", type=int, default=60,
                    help="refuse to run if the diff exposes more than this many symbols")
    ap.add_argument("--spread", type=float, default=0.05,
                    help="drop symbols appearing in more than this fraction of the corpus "
                         "(they do not discriminate); default 0.05")
    ap.add_argument("--node-run", type=Path, help="a node run dir, for the authoritative file list")
    ap.add_argument("--bun-run", type=Path, help="a bun run dir, for the authoritative file list")
    args = ap.parse_args()

    lines = changed_lines(args.rev_range)
    if not lines:
        print("impact_gate: no added lines found -- nothing to gate", file=sys.stderr)
        return 2

    syms, refused = symbols(lines, args.extra_symbol)
    print(f"impact_gate: {len(lines)} added lines -> {len(syms)} usable symbols "
          f"({len(refused)} refused as too common)")
    if len(syms) > args.max_symbols:
        print(f"impact_gate: {len(syms)} symbols exceeds --max-symbols {args.max_symbols}. "
              f"A diff this broad is what a FULL run is for -- say so explicitly rather "
              f"than gating on a set this large.", file=sys.stderr)
        return 3
    if syms:
        print("  symbols: " + ", ".join(sorted(syms)[:20]) +
              (f", +{len(syms) - 20} more" if len(syms) > 20 else ""))

    total = 0
    for label, corpus, out, run in (("node", NODE_CORPUS, args.out, args.node_run),
                                    ("bun", BUN_CORPUS, args.bun_out, args.bun_run)):
        hits, freq = select(syms, corpus, args.explain, run, args.spread)
        dropped = {s: n for s, n in freq.items() if n > max(1, int(
            len(corpus_files(corpus, run)) * args.spread))}
        total += len(hits)
        print(f"  {label}: {len(hits)} corpus files reachable")
        if dropped:
            top = sorted(dropped.items(), key=lambda kv: -kv[1])[:5]
            print(f"      dropped as non-discriminating: " +
                  ", ".join(f"{s}({n})" for s, n in top))
        if args.explain:
            for f, ms in sorted(hits.items())[:15]:
                print(f"      {f}  <- {', '.join(sorted(ms)[:4])}")
            if len(hits) > 15:
                print(f"      ... +{len(hits) - 15} more")
        if out:
            out.write_text("\n".join(sorted(hits)) + ("\n" if hits else ""))
            print(f"      written to {out}")

    print("\nCAVEAT, printed every time so a clean gate cannot imply more than it proved:")
    print("  this finds files that MENTION a changed name. It cannot see a change")
    print("  observable only through behaviour with no name in it -- ordering, timing,")
    print("  GC. For those, add --extra-symbol or run the full corpus and say why.")
    if total == 0:
        print("  0 files selected: either the change is unobservable from the corpus,")
        print("  or the symbols are wrong. Do NOT read this as 'no regressions'.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
