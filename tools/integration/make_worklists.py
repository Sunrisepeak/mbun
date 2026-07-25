#!/usr/bin/env python3
"""make_worklists — cut a corpus run into small, disjoint, named agent worklists.

WHY THIS EXISTS: the remaining work has stopped being mechanism-shaped. Measured
on the round-11 run of the node corpus, 1419 non-passing files carry **540
distinct failure signatures** — 2.6 files each. 41% sit in clusters of four files
or fewer and 31% are singletons, while only ~20% sit in clusters big enough to
brief as a mechanism at all (and even those, `mustCall` and `timeout`, are
umbrellas over many unrelated causes).

Hunting for mechanisms was the right strategy while mechanisms existed: it took a
wave from 3.7% of target to 150%. Against a long tail it is the wrong tool — the
search costs more than the repair. What a long tail needs is throughput, and
throughput means many small agents each handed a short list of *specific files*
rather than a subsystem to explore.

Two properties this enforces, both learned the expensive way:

  DISJOINT — two agents editing the same area produce merge conflicts and
  un-attributable measurements. Lists are cut so no file appears twice, and files
  are grouped by subsystem so their likely edit sites stay apart.

  NAMED — every file carries its extracted signature and confidence. An agent
  that has to re-derive why a file fails is paying the triage cost this project
  already eliminated once.

Files in MANIFESTATION or UNSPLIT groups are included but flagged: their
signature names no contract, so the agent must read the file itself. They are
deliberately not hidden — a quarter of the remaining corpus is in that state and
pretending otherwise would just move the surprise later.

Usage:
    make_worklists.py --run target/integration/r11-final --corpus node \\
        --out target/integration/worklists --agents 8 --per-agent 12
    make_worklists.py --run <dir> --corpus node --subsystem fs --agents 2

Each list is written as `<out>/<name>.txt` (paths, one per line, ready for
`node_corpus_runner.py --files`) alongside `<name>.md` (the same files with their
signature, confidence and bucket, ready to paste into a brief).
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import sys
from pathlib import Path


def load_signature_module(root: Path):
    path = root / "tools" / "integration" / "failure_signature.py"
    spec = importlib.util.spec_from_file_location("failure_signature", path)
    module = importlib.util.module_from_spec(spec)
    # @dataclass resolves annotations via sys.modules[cls.__module__]
    sys.modules["failure_signature"] = module
    spec.loader.exec_module(module)
    return module


SUBSYSTEM_RULES = (
    ("http2", lambda n: n.startswith("test-http2")),
    ("tls", lambda n: n.startswith("test-tls") or n.startswith("test-https")),
    ("http", lambda n: n.startswith("test-http")),
    ("fs", lambda n: n.startswith("test-fs")),
    ("stream", lambda n: n.startswith("test-stream")),
    ("net-dgram", lambda n: n.startswith("test-net") or n.startswith("test-dgram")),
    ("child-cluster", lambda n: n.startswith("test-child") or n.startswith("test-cluster")),
    ("worker", lambda n: n.startswith("test-worker")),
    ("crypto", lambda n: n.startswith("test-crypto")),
    ("module", lambda n: n.startswith("test-module") or n.startswith("test-require")
     or n.startswith("test-esm")),
    ("process", lambda n: n.startswith("test-process") or n.startswith("test-stdio")
     or n.startswith("test-signal")),
    ("util-url", lambda n: n.startswith("test-util") or n.startswith("test-url")
     or n.startswith("test-whatwg")),
)


def subsystem_of(name: str) -> str:
    for label, match in SUBSYSTEM_RULES:
        if match(name):
            return label
    return "misc"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--corpus", required=True, choices=("bun", "node"))
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--agents", type=int, default=8)
    ap.add_argument("--per-agent", type=int, default=12)
    ap.add_argument("--subsystem", help="only cut lists from this subsystem")
    ap.add_argument("--exclude-timeouts", action="store_true",
                    help="leave timeouts out; they are triaged by dump shape, not log text")
    ap.add_argument("--root", type=Path, default=Path.cwd())
    args = ap.parse_args()

    mod = load_signature_module(args.root.resolve())
    sign = mod.bun_signature if args.corpus == "bun" else mod.node_signature
    rows = mod.load_rows(args.run, args.corpus)

    # (subsystem, signature) -> [(path, confidence)]
    grouped: dict[str, list[tuple[str, str, str]]] = collections.defaultdict(list)
    for path, classification, log in rows:
        if args.exclude_timeouts and classification in ("timeout", "oom-kill"):
            continue
        log_path = args.run / log
        if not log_path.exists():
            continue
        sig = (mod.timeout_signature() if classification in ("timeout", "oom-kill")
               else sign(log_path.read_text(encoding="utf-8", errors="replace")))
        name = path.rsplit("/", 1)[-1]
        sub = subsystem_of(name)
        if args.subsystem and sub != args.subsystem:
            continue
        grouped[sub].append((path, sig.key, sig.confidence))

    # Keep each agent inside ONE subsystem so their edit sites stay apart, and
    # order a list by signature so related files sit together within it — a repair
    # for one often covers its neighbour.
    # ROUND-ROBIN across subsystems, not largest-first. Taking the biggest
    # subsystem first handed all seven lists to `misc`, which defeats the whole
    # point: the lists are cut by subsystem so concurrent agents edit different
    # areas, and seven agents inside one bucket is the merge-conflict shape this
    # is meant to avoid.
    per_sub: dict[str, list[list[tuple[str, str, str]]]] = {}
    for sub, entries in grouped.items():
        items = sorted(entries, key=lambda t: (t[1], t[0]))
        chunks = [items[i:i + args.per_agent] for i in range(0, len(items), args.per_agent)]
        per_sub[sub] = [c for c in chunks if len(c) >= 2]

    lists: list[tuple[str, list[tuple[str, str, str]]]] = []
    round_index = 0
    # Largest subsystems first WITHIN a round, so a round of N agents spreads over
    # the N busiest areas before any area gets a second agent.
    order = sorted(per_sub, key=lambda s: -len(grouped[s]))
    while len(lists) < args.agents and any(len(per_sub[s]) > round_index for s in order):
        for sub in order:
            if len(lists) >= args.agents:
                break
            if len(per_sub[sub]) > round_index:
                lists.append((f"{sub}-{round_index + 1}", per_sub[sub][round_index]))
        round_index += 1

    args.out.mkdir(parents=True, exist_ok=True)
    seen: set[str] = set()
    for name, chunk in lists:
        paths = [p for p, _, _ in chunk]
        overlap = seen & set(paths)
        if overlap:  # cannot happen by construction; assert it anyway
            raise SystemExit(f"worklists overlap on {sorted(overlap)[:3]} — refusing to write")
        seen |= set(paths)
        (args.out / f"{name}.txt").write_text("".join(p + "\n" for p in paths), encoding="utf-8")
        lines = [f"# worklist {name} — {len(chunk)} files", ""]
        for path, key, confidence in chunk:
            flag = "" if confidence in ("CAUSE", "CLASS") else \
                   "  ⚠ signature names no contract — read the file"
            lines.append(f"- `{path.rsplit('/', 1)[-1]}` — [{confidence}] {key}{flag}")
        (args.out / f"{name}.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        named = sum(1 for _, _, c in chunk if c in ("CAUSE", "CLASS"))
        print(f"{name:16} {len(chunk):3} files  ({named} with a named cause)  -> {args.out}/{name}.txt")

    print(f"\n{len(lists)} disjoint worklists, {len(seen)} files total, no file in two lists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
