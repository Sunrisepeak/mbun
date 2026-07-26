#!/usr/bin/env bash
# check_submodule_gitlinks — refuse a tree where a submodule became a symlink.
#
# WHY THIS EXISTS: every agent worktree wires `compat/bun` and `compat/node` as
# SYMLINKS to the primary checkout, because git leaves a submodule directory
# empty in a linked worktree. That is correct locally and invisible day to day —
# but if the symlink is ever staged, the commit replaces the submodule gitlink
# (mode 160000, pointing at an upstream commit) with a mode-120000 blob holding a
# path from one developer's machine.
#
# It has happened once already, in a merge, and nothing caught it: the diff
# summary said `mode change 160000 => 120000` on one line between two source
# files, and both corpora still resolved locally because the symlink was valid
# HERE. On any other checkout `compat/node/test` would simply not exist, and the
# corpus runners would report a corpus of zero files rather than an error.
#
# So this is cheap insurance against a class of damage that is silent for whoever
# creates it and total for everyone else.
#
# Usage:
#   check_submodule_gitlinks.sh            # check the index (pre-commit)
#   check_submodule_gitlinks.sh <rev>      # check a commit's tree
set -uo pipefail

root=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "check_submodule_gitlinks: not a git repository" >&2
  exit 2
}
cd "$root" || exit 2

rev=${1:-}
paths=(compat/bun compat/node)
bad=0

for path in "${paths[@]}"; do
  if [ -n "$rev" ]; then
    entry=$(git ls-tree "$rev" -- "$path" 2>/dev/null)
    mode=${entry%% *}
    where="$rev"
  else
    entry=$(git ls-files -s -- "$path" 2>/dev/null)
    mode=${entry%% *}
    where="the index"
  fi

  if [ -z "$entry" ]; then
    echo "check_submodule_gitlinks: $path is absent from $where" >&2
    bad=1
    continue
  fi
  if [ "$mode" != "160000" ]; then
    echo "check_submodule_gitlinks: $path has mode $mode in $where, expected 160000 (a submodule gitlink)." >&2
    if [ "$mode" = "120000" ]; then
      echo "  It is staged as a SYMLINK — almost certainly a worktree's compat/ link that got added." >&2
      echo "  Restore it with the upstream commit it should point at:" >&2
      echo "    git update-index --cacheinfo 160000,<submodule-sha>,$path" >&2
      echo "  (take <submodule-sha> from a commit that predates the damage: git ls-tree <good-rev> $path)" >&2
    fi
    bad=1
  fi
done

if [ "$bad" != 0 ]; then
  exit 1
fi
echo "check_submodule_gitlinks: clean"
