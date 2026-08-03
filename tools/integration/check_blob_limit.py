#!/usr/bin/env python3
"""Fail before GCC does when an embedded JS blob outgrows the constexpr limit.

mbun embeds JavaScript in C++ raw string literals assigned to
`inline constexpr std::string_view`. Building that view makes the compiler walk
the literal at compile time, and GCC caps that walk at `-fconstexpr-loop-limit`
(default 262144). One character past it, the build fails with

    error: 'constexpr' loop iteration count exceeds limit of 262144
      (use '-fconstexpr-loop-limit=' to increase the limit)

pointing inside `<char_traits.h>` -- not at the blob, not at the file the author
edited. A W45 lane lost time to exactly this: adding a few lines of JS to
`process_web.cppm` failed the build on source that looked pristine, and the
diagnostic named a standard header.

The blob that tripped it now sits **318 characters** below the ceiling, so the
next person to add a comment there hits the same wall. This check names the file,
the symbol and the remaining headroom, so the failure arrives as a sentence
instead of an archaeology exercise.

    tools/integration/check_blob_limit.py            # fail over the limit, warn when close
    tools/integration/check_blob_limit.py --margin 0 # report only true failures

Exit 0 = every blob fits (warnings may still be printed), 1 = at least one is over.

THE REAL FIX for a blob near the ceiling is to split it into two views
concatenated at runtime, or to stop making the view `constexpr`. Raising
`-fconstexpr-loop-limit` also works and may be the right call, but it has not
been measured here for compile-time cost, so this check does not assume it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# GCC's default -fconstexpr-loop-limit. The walk is one iteration per character.
LIMIT = 262144
DECL = re.compile(r'inline\s+constexpr\s+std::string_view\s+(\w+)\s*=\s*R"(\w*)\(')


def blobs(root: Path):
    """Yield (path, symbol, length) for every constexpr string_view raw literal."""
    for path in sorted(root.rglob("*.cppm")):
        try:
            src = path.read_text(errors="ignore")
        except OSError:
            continue
        for m in DECL.finditer(src):
            symbol, tag = m.group(1), m.group(2)
            end = src.find(f"){tag}\"", m.end())
            if end < 0:
                continue  # unterminated literal is the compiler's problem, not ours
            yield path, symbol, end - m.end()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default="modules", type=Path,
                    help="directory to scan (default: modules)")
    ap.add_argument("--margin", type=int, default=8192,
                    help="warn when a blob is within this many characters of the limit")
    args = ap.parse_args()

    over, near = [], []
    for path, symbol, size in blobs(args.root):
        if size > LIMIT:
            over.append((path, symbol, size))
        elif size > LIMIT - args.margin:
            near.append((path, symbol, size))

    for path, symbol, size in near:
        print(f"check_blob_limit: WARNING {path}:{symbol} is {size} chars, "
              f"{LIMIT - size} below the {LIMIT} constexpr limit -- "
              f"adding to it will fail the build inside <char_traits.h>", file=sys.stderr)

    for path, symbol, size in over:
        print(f"check_blob_limit: {path}:{symbol} is {size} chars, "
              f"{size - LIMIT} OVER the {LIMIT} constexpr limit. GCC will fail with "
              f"\"'constexpr' loop iteration count exceeds limit\" pointing inside "
              f"<char_traits.h>. Split the blob into two views concatenated at runtime, "
              f"or drop constexpr on this view.", file=sys.stderr)

    if over:
        return 1
    print(f"check_blob_limit: clean ({len(near)} blob(s) within {args.margin} of the limit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
