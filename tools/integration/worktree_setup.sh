#!/usr/bin/env bash
# worktree_setup.sh -- create or re-point a parallel-agent worktree with the
# compat corpora correctly wired.
#
# WHY THIS EXISTS: `compat/bun` and `compat/node` are submodules holding tens of
# GB of upstream checkout. A worktree must not re-materialise them, so the
# convention is to symlink both at the main checkout's copies. Doing that by
# hand is a trap: git leaves an EMPTY submodule directory in a fresh worktree,
# and `ln -sfn <target> <wt>/compat/node` then silently creates the link INSIDE
# it (`<wt>/compat/node/node -> .../compat/node`) instead of replacing it. The
# corpus path `compat/node/test/parallel` then does not exist, and the failure
# surfaces far away -- as "no test files selected", or as a runner that scores a
# subset it never actually ran. That cost two agents real time before it was
# understood, so the wiring is a tested script now, not a remembered incantation.
#
# Usage:
#   tools/integration/worktree_setup.sh <worktree-path> <branch> [start-point]
#
#   worktree-path  where the worktree lives (created if absent, re-pointed if present)
#   branch         branch to create/reset (git checkout -B)
#   start-point    commit/branch to base it on (default: HEAD of the main checkout)
#
# Idempotent: safe to re-run on an existing worktree. It never touches tracked
# files and never deletes a worktree's build output.
set -euo pipefail

usage() { echo "usage: $0 <worktree-path> <branch> [start-point]" >&2; exit 2; }
[ $# -ge 2 ] || usage

main_root=$(git rev-parse --show-toplevel)
worktree=$1
branch=$2
start_point=${3:-HEAD}

# Resolve the start point in the main checkout before touching the worktree, so
# a typo fails here rather than half-way through a re-point.
start_sha=$(git -C "$main_root" rev-parse --verify "${start_point}^{commit}")

if [ -d "$worktree/.git" ] || [ -f "$worktree/.git" ]; then
  # Re-point an existing worktree. Keep target/ (the incremental build cache is
  # expensive) and keep the compat symlinks, which are untracked here.
  git -C "$worktree" checkout -q --detach
  git -C "$worktree" reset -q --hard "$start_sha"
  git -C "$worktree" clean -qfd -e target -e compat
  git -C "$worktree" checkout -q -B "$branch" "$start_sha"
else
  git -C "$main_root" worktree add -q --detach "$worktree" "$start_sha"
  git -C "$worktree" checkout -q -B "$branch" "$start_sha"
fi

# Wire the corpora. `ln -sfn` alone is NOT enough: if the path is an existing
# directory the link lands inside it. Remove whatever is there first -- but only
# when it is a symlink or an EMPTY directory, so a real populated submodule
# checkout in a worktree is never destroyed by a setup script.
for name in bun node; do
  target="$main_root/compat/$name"
  link="$worktree/compat/$name"
  [ -e "$target" ] || { echo "$0: missing $target -- is the submodule checked out?" >&2; exit 1; }
  mkdir -p "$worktree/compat"
  if [ -L "$link" ]; then
    rm -f "$link"
  elif [ -d "$link" ]; then
    if [ -n "$(ls -A "$link" 2>/dev/null)" ]; then
      # Populated: either a real checkout (leave it) or a previous run's
      # inside-the-directory mistake (repairable).
      if [ -L "$link/$name" ]; then
        rm -rf "$link"
      else
        echo "$0: $link is a populated directory, refusing to replace it" >&2
        continue
      fi
    else
      rmdir "$link"
    fi
  fi
  ln -s "$target" "$link"
done

# Drop the generated build graph. It is keyed on the module list at generation
# time, so re-pointing a worktree at a commit that ADDS a module partition
# leaves ninja unaware of it and `mcpp build` dies with
#   failed to read compiled module: ... <partition>.gcm
#   imports must be built before being imported
# which reads like a source error and is not one. Deleting it forces a
# regeneration on the next build and costs far less than the --no-cache full
# rebuild people reach for instead. Object files under target/ are kept, so the
# incremental cache survives. This bit three separate agents in round 9.
find "$worktree/target" -name build.ninja -delete 2>/dev/null || true

# Prove the wiring rather than assume it: the corpus path the runners use must
# resolve to real files, here and now.
for probe in "compat/node/test/parallel" "compat/node/test/common/index.js" "compat/bun/test"; do
  [ -e "$worktree/$probe" ] || { echo "$0: wiring failed -- $worktree/$probe does not resolve" >&2; exit 1; }
done
count=$(find "$worktree/compat/node/test/parallel" -maxdepth 1 -name 'test-*.js' | wc -l)
[ "$count" -gt 0 ] || { echo "$0: wiring failed -- no test files under compat/node/test/parallel" >&2; exit 1; }

echo "$worktree -> $branch @ $(git -C "$worktree" rev-parse --short HEAD) (corpus ok: $count node parallel files)"
