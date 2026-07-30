#!/usr/bin/env python3
"""conflict_census.py -- find cross-corpus behaviour conflicts mechanically.

A *cross-corpus conflict* is one call site where compat/node and compat/bun
demand different observable behaviour from ONE binary, with no discriminator
available at the call site. Every one of them is a hard ceiling: the campaign
can green the node file or the bun file, never both.

Six were found by stumbling into them one file at a time. This tool finds the
rest without reading 3857 corpus files by hand.

Two independent passes:

  1. LEDGER pass -- `tools/integration/struck.tsv` already carries verdicted
     `COSTS_GREEN` rows. Some are cross-corpus conflicts, some are ordinary
     "implemented it and it lost files inside one corpus". Classify them.

  2. MINE pass -- the mechanical one. The key observation is that mbun's
     CURRENT behaviour is the tiebreaker:

        - a file fails because it asserts value A;
        - the log of that failing run contains value B, which is what mbun
          actually produced;
        - if B is *asserted by a green file in the other corpus*, then B is
          pinned: making the failing file green means breaking that green one.

     So a candidate is a token B with
        B observed in the failure log of a failing file in corpus X,
        B NOT written in that failing file's own source (i.e. X does not want
          B -- it is mbun's output, not the expectation),
        B asserted in >=1 GREEN file of corpus Y.

     Tokens are restricted to things a test can pin exactly: OpenSSL error
     codes, ERR_* / DOMException names, and quoted message strings. Generic
     English and paths are dropped, because they collide across the corpora for
     reasons that have nothing to do with behaviour.

The output is a *candidate* list. A candidate is SUSPECTED until both sides'
assertions have been read; a candidate with a discriminator (argv0, entry
extension, an explicit flag) is CLEARED and is not a ceiling. Confirmed
conflicts belong in struck.tsv with verdict COSTS_GREEN.

Usage:
    conflict_census.py ledger
    conflict_census.py mine  --node-run DIR --bun-run DIR [--min-len 10]
    conflict_census.py ceiling --node-run DIR --bun-run DIR
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STRUCK = Path(__file__).resolve().parent / "struck.tsv"

# --------------------------------------------------------------------------
# token extraction
# --------------------------------------------------------------------------

# What a corpus file can pin byte-for-byte. Ordered most-specific first; the
# comment on each says why it earns its place, because a loose pattern here
# turns the whole census into noise.
TOKEN_PATTERNS = (
    # OpenSSL/BoringSSL packed error codes. `error:1E08010C:DECODER routines`
    # vs `error:06000066:public key routines` is conflict #1 verbatim.
    re.compile(r"error:[0-9A-Fa-f]{8}:[^\"'`\n\\]{0,60}"),
    # node error codes and DOM exception names -- the single most-asserted
    # class of value in both corpora.
    re.compile(r"\b(?:ERR_[A-Z0-9_]{3,}|[A-Z][A-Za-z]*Error|QuotaExceededError"
               r"|DOMException|MODULE_NOT_FOUND|E[A-Z]{3,7})\b"),
    # message text: a quoted run with a space in it. Length-gated by --min-len.
    re.compile(r"[\"'`]([^\"'`\n\\]{8,160})[\"'`]"),
)

# Tokens that are true of half the corpus and carry no behavioural claim.
# Every one of these was observed colliding in >100 files during development.
STOPWORDS = {
    "Error", "TypeError", "RangeError", "SyntaxError", "AssertionError",
    "ReferenceError", "EvalError", "URIError", "AggregateError",
    "use strict", "utf-8", "utf8", "ERR_ASSERTION", "ERR_INVALID_ARG_TYPE",
    "ERR_INVALID_ARG_VALUE", "ERR_MISSING_ARGS", "ERR_OUT_OF_RANGE",
}

# Anything that names a place on this machine, a build id, or a clock reading
# is per-run noise, not a pinned value.
NOISE = re.compile(
    r"(?:^|[/\\])(?:home|tmp|usr|var|proc|dev|Users)[/\\]"
    r"|\.(?:js|ts|mjs|cjs|json|log|node|so|txt|cppm|inc|rs)\b"
    r"|^[0-9.\s:+-]+$"
    r"|\bv?\d+\.\d+\.\d+\b"
    r"|[0-9a-f]{12,}"
    r"|node_modules|compat/|target/"
)


def tokens(text: str, min_len: int = 10) -> set[str]:
    """Distinctive, pinnable literals in `text`."""
    out: set[str] = set()
    for pat in TOKEN_PATTERNS:
        for m in pat.finditer(text):
            tok = (m.group(1) if m.groups() else m.group(0)).strip()
            if not tok or tok in STOPWORDS:
                continue
            if NOISE.search(tok):
                continue
            # A message needs a space (it is prose); a code does not.
            is_code = tok.startswith("error:") or re.fullmatch(
                r"[A-Z][A-Za-z0-9_]*", tok)
            if not is_code:
                if len(tok) < min_len or " " not in tok:
                    continue
            out.add(tok)
    return out


ASSERT_CTX = re.compile(
    r"assert|expect|toBe|toEqual|toThrow|toMatch|toContain|strictEqual"
    r"|deepEqual|\bcode\s*:|\bmessage\s*:|\bname\s*:|throws|rejects")


def asserted_tokens(src: str, min_len: int = 10) -> set[str]:
    """Tokens a file pins in an ASSERTION context.

    A green file only *pins* mbun's behaviour if it checks the value. A message
    that appears in a comment, or as an input string, constrains nothing -- and
    including those turned an early run of this pass into 400 false candidates.
    Context is the line itself plus two lines either side, because both corpora
    routinely wrap the expected value onto its own line.
    """
    lines = src.splitlines()
    out: set[str] = set()
    for i, line in enumerate(lines):
        window = "\n".join(lines[max(0, i - 2): i + 3])
        if not ASSERT_CTX.search(window):
            continue
        out |= tokens(line, min_len)
    return out


# --------------------------------------------------------------------------
# run loading
# --------------------------------------------------------------------------

class Corpus:
    def __init__(self, name: str, run_dir: Path, root: Path,
                 class_col: int, log_col: int, green_value: str):
        self.name = name
        self.run_dir = run_dir
        self.root = root
        self.green: dict[str, Path] = {}   # corpus path -> log path
        self.failing: dict[str, Path] = {}
        rows = (run_dir / "results.tsv").read_text(
            encoding="utf-8", errors="replace").splitlines()
        for row in rows[1:]:
            f = row.split("\t")
            if len(f) <= max(class_col, log_col):
                continue
            path, cls, log = f[0], f[class_col], f[log_col]
            logp = run_dir / log
            if cls == green_value:
                self.green[path] = logp
            elif cls in ("fail", "test-failure"):
                self.failing[path] = logp

    def read(self, rel: str) -> str:
        p = self.root / rel
        try:
            return p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""


def load(node_run: Path, bun_run: Path, root: Path) -> tuple[Corpus, Corpus]:
    node = Corpus("node", node_run, root, class_col=2, log_col=4,
                  green_value="pass")
    bun = Corpus("bun", bun_run, root, class_col=6, log_col=8,
                 green_value="green")
    return node, bun


# --------------------------------------------------------------------------
# pass 1 -- the ledger
# --------------------------------------------------------------------------

# Phrases a lane writes when it has actually established that the two corpora
# want different things. These are the words already in struck.tsv, not words
# invented here.
CROSS_MARKS = re.compile(
    r"cross-corpus|no discriminator|no caller-side discriminator"
    r"|same input, no|indistinguishable|for 1 green node file"
    r"|for \d+ green node file|one binary|ONE binary|both corpora",
    re.IGNORECASE)


def read_struck(path: Path = STRUCK) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split("\t")
        if len(f) < 5 or f[2] == "verdict":
            continue
        rows.append({"area": f[0], "target": f[1], "verdict": f[2],
                     "cost": f[3], "evidence": f[4]})
    return rows


def is_cross(r: dict) -> bool:
    """A COSTS_GREEN row is cross-corpus when the loss lands in the OTHER
    corpus than the one the work was for. `area` names the corpus the change
    serves; a cost counted in the other corpus' files is the signature. The
    phrase marks catch the rows whose cost column is prose."""
    blob = r["target"] + " " + r["cost"] + " " + r["evidence"]
    if CROSS_MARKS.search(blob):
        return True
    other = "bun" if r["area"] != "bun" else "node"
    return bool(re.search(rf"\b{other}\b", r["cost"], re.IGNORECASE))


def cmd_ledger(_args) -> int:
    rows = read_struck()
    costs = [r for r in rows if r["verdict"] == "COSTS_GREEN"]
    cross, single = [], []
    for r in costs:
        (cross if is_cross(r) else single).append(r)
    print(f"struck.tsv rows: {len(rows)}   COSTS_GREEN: {len(costs)}")
    print(f"  cross-corpus conflict (node vs bun, one binary): {len(cross)}")
    print(f"  single-corpus loss (no conflict, just a bad trade): {len(single)}")
    print()
    for label, group in (("CROSS-CORPUS", cross), ("SINGLE-CORPUS", single)):
        print(f"--- {label} ---")
        for r in group:
            print(f"  [{r['area']}] {r['target']}")
            print(f"      cost: {r['cost']}")
        print()
    return 0


# --------------------------------------------------------------------------
# pass 2 -- the miner
# --------------------------------------------------------------------------

def mine(node: Corpus, bun: Corpus, min_len: int,
         max_pin: int) -> list[dict]:
    """Candidates where one corpus' failure log carries a value the other
    corpus pins in a green file."""
    # index: token -> green files that assert it, per corpus
    pinned: dict[str, dict[str, set[str]]] = {
        "node": defaultdict(set), "bun": defaultdict(set)}
    for corp in (node, bun):
        for rel in corp.green:
            for t in asserted_tokens(corp.read(rel), min_len):
                pinned[corp.name][t].add(rel)

    cands: dict[tuple[str, str], dict] = {}
    for demand, pin in ((node, bun), (bun, node)):
        pinmap = pinned[pin.name]
        for rel, logp in demand.failing.items():
            try:
                log = logp.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            src = demand.read(rel)
            observed = tokens(log, min_len)
            wanted = tokens(src, min_len)
            for t in observed - wanted:
                holders = pinmap.get(t)
                if not holders:
                    continue
                # A value pinned by half the other corpus is a shared
                # convention, not a conflict -- mbun already satisfies it.
                if len(holders) > max_pin:
                    continue
                key = (demand.name, t)
                c = cands.setdefault(key, {
                    "demanding": demand.name, "pinning": pin.name,
                    "token": t, "demanding_files": set(),
                    "pinning_files": set()})
                c["demanding_files"].add(rel)
                c["pinning_files"] |= holders
    out = sorted(cands.values(),
                 key=lambda c: (-len(c["demanding_files"]),
                                -len(c["pinning_files"]), c["token"]))
    return out


def cmd_mine(args) -> int:
    node, bun = load(Path(args.node_run), Path(args.bun_run), Path(args.root))
    print(f"node: {len(node.green)} green, {len(node.failing)} failing",
          file=sys.stderr)
    print(f"bun:  {len(bun.green)} green, {len(bun.failing)} failing",
          file=sys.stderr)
    cands = mine(node, bun, args.min_len, args.max_pin)
    lines = ["demanding\tpinning\tn_demanding\tn_pinning\ttoken"
             "\tdemanding_files\tpinning_files"]
    for c in cands:
        lines.append("\t".join([
            c["demanding"], c["pinning"],
            str(len(c["demanding_files"])), str(len(c["pinning_files"])),
            c["token"].replace("\t", " "),
            ",".join(sorted(c["demanding_files"])[:8]),
            ",".join(sorted(c["pinning_files"])[:8]),
        ]))
    text = "\n".join(lines) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"{len(cands)} candidates -> {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    print(f"CANDIDATES: {len(cands)}", file=sys.stderr)
    print("A candidate is SUSPECTED, not confirmed. Read both assertions "
          "before counting it against the ceiling; a candidate with a "
          "discriminator is CLEARED.", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# pass 3 -- near-miss assertion pairs (source only, no run needed)
# --------------------------------------------------------------------------

# The log-driven pass can only see files a run covered, and it only fires when
# the value reaches the log. This pass needs neither: it compares what the two
# corpora WRITE DOWN. Two spellings of the same message, one per corpus, is the
# signature of every message-shaped conflict found so far
# ("Maximum call stack size exceeded" vs the same with JSC's trailing period;
# node's `Cannot find module` vs bun's `Module not found`).

WORD = re.compile(r"[A-Za-z0-9_]+")


def norm(msg: str) -> str:
    return " ".join(WORD.findall(msg.lower()))


def corpus_msgs(files, read, min_len: int) -> dict[str, set[str]]:
    """raw message -> files asserting it."""
    idx: dict[str, set[str]] = defaultdict(set)
    for rel in files:
        for t in asserted_tokens(read(rel), min_len):
            if " " in t and not t.startswith("error:"):
                idx[t].add(rel)
    return idx


def near_pairs(a: dict[str, set[str]], b: dict[str, set[str]],
               min_words: int) -> list[tuple[str, str, set[str], set[str]]]:
    """Messages equal after normalisation but not byte-equal.

    Deliberately NOT a fuzzy-distance search. Edit distance over two corpora of
    tens of thousands of strings produces mostly pairs that differ because they
    are about different things; normalisation-equality produces pairs that differ
    only in punctuation, case or spacing -- which is precisely what a corpus
    pins byte-for-byte and an implementation cannot serve twice.
    """
    by_norm_a: dict[str, set[str]] = defaultdict(set)
    for raw in a:
        by_norm_a[norm(raw)].add(raw)
    out = []
    for raw_b in b:
        key = norm(raw_b)
        if len(key.split()) < min_words:
            continue
        for raw_a in by_norm_a.get(key, ()):
            if raw_a != raw_b:
                out.append((raw_a, raw_b, a[raw_a], b[raw_b]))
    return out


def cmd_pairs(args) -> int:
    root = Path(args.root)
    node, bun = load(Path(args.node_run), Path(args.bun_run), root)

    # node side: every file on disk, because the node run is a gate subset
    # (1955 of 4433) and a conflict outside it is still a conflict.
    node_all = sorted(
        p.relative_to(root).as_posix()
        for p in (root / "compat/node/test/parallel").rglob("test-*")
        if p.is_file() and p.suffix in {".js", ".mjs", ".cjs"})
    bun_all = sorted(
        p.relative_to(root).as_posix()
        for p in (root / "compat/bun/test").rglob("*")
        if p.is_file() and p.suffix in {".js", ".ts", ".mjs", ".cjs", ".jsx",
                                        ".tsx"}
        and "node_modules" not in p.as_posix())

    n_idx = corpus_msgs(node_all, node.read, args.min_len)
    b_idx = corpus_msgs(bun_all, bun.read, args.min_len)
    print(f"node messages {len(n_idx)}  bun messages {len(b_idx)}",
          file=sys.stderr)

    pairs = near_pairs(n_idx, b_idx, args.min_words)
    lines = ["node_msg\tbun_msg\tn_node_files\tn_bun_files\tnode_in_run"
             "\tbun_green\tnode_files\tbun_files"]
    for raw_a, raw_b, fa, fb in sorted(pairs, key=lambda p: -len(p[3])):
        in_run = sum(1 for f in fa if f in node.green or f in node.failing)
        bgreen = sum(1 for f in fb if f in bun.green)
        lines.append("\t".join([
            raw_a.replace("\t", " "), raw_b.replace("\t", " "),
            str(len(fa)), str(len(fb)), str(in_run), str(bgreen),
            ",".join(sorted(fa)[:6]), ",".join(sorted(fb)[:6])]))
    text = "\n".join(lines) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    print(f"NEAR-MISS PAIRS: {len(pairs)}", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# ceiling arithmetic
# --------------------------------------------------------------------------

def cmd_ceiling(args) -> int:
    node, bun = load(Path(args.node_run), Path(args.bun_run), Path(args.root))
    rows = read_struck()
    blocked = 0
    for r in rows:
        if r["verdict"] in ("COSTS_GREEN", "BLOCKED"):
            blocked += 1
    n_tot = len(node.green) + len(node.failing)
    print(f"node green {len(node.green)}  failing {len(node.failing)}")
    print(f"bun  green {len(bun.green)}   failing {len(bun.failing)}")
    print(f"struck rows with a hard verdict: {blocked}")
    print("Ceiling arithmetic needs the per-conflict file counts from the "
          "confirmed list; this subcommand only prints the denominators.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ledger", help="classify struck.tsv COSTS_GREEN rows")
    p.set_defaults(fn=cmd_ledger)

    for name, fn in (("mine", cmd_mine), ("pairs", cmd_pairs),
                     ("ceiling", cmd_ceiling)):
        p = sub.add_parser(name)
        p.add_argument("--node-run", required=True)
        p.add_argument("--bun-run", required=True)
        p.add_argument("--root", default=str(ROOT),
                       help="repo root the corpus paths resolve against")
        p.add_argument("--min-len", type=int, default=10)
        p.add_argument("--max-pin", type=int, default=12,
                       help="drop tokens pinned by more green files than this")
        p.add_argument("--min-words", type=int, default=4,
                       help="pairs: shortest message worth comparing, in words")
        p.add_argument("--out")
        p.set_defaults(fn=fn)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
