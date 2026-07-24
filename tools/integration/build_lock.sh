#!/usr/bin/env bash
# build_lock.sh -- serialise expensive builds ACROSS parallel agent worktrees.
#
# WHY: `mcpp build`'s link phase spawns many `ld` processes, each ~1 GB RSS for
# this binary. One build is fine on a 32-core/62 GB box. Three agents whose link
# phases happen to overlap are not: a measured peak during round 9 hit load 41,
# drove available memory from ~40 GB to 19 GB and filled the 2 GB swapfile. The
# next step past that is the machine freezing -- the exact failure the bounded
# execution layer exists to prevent, arriving through the build instead of
# through a test.
#
# Per-agent discipline ("never run two builds at once") cannot fix this, because
# no agent can see the other worktrees. The lock is shared state in the main
# checkout, so it works across worktrees and across separate agent processes.
#
# Usage:
#   tools/integration/build_lock.sh mcpp build
#   tools/integration/build_lock.sh -- mcpp build --verbose
#
# Env:
#   MBUN_BUILD_SLOTS   how many builds may run at once (default 1)
#   MBUN_BUILD_WAIT    seconds to wait for a slot before giving up (default 3600)
#
# Exit code is the wrapped command's, or 75 (EX_TEMPFAIL) if no slot came free.
set -uo pipefail

[ $# -gt 0 ] || { echo "usage: $0 [--] <command...>" >&2; exit 2; }
[ "$1" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: $0 [--] <command...>" >&2; exit 2; }

slots="${MBUN_BUILD_SLOTS:-1}"
wait_sec="${MBUN_BUILD_WAIT:-3600}"

# Anchor the lock in the MAIN checkout, never the worktree: `git rev-parse
# --git-common-dir` resolves to the shared .git for every linked worktree, which
# is exactly the scope we need the mutual exclusion over.
common_dir=$(git rev-parse --git-common-dir 2>/dev/null) || {
  echo "$0: not in a git repository" >&2; exit 2; }
lock_dir="$(cd "$common_dir" && pwd)/mbun-build-locks"
mkdir -p "$lock_dir"

start=$(date +%s)
while :; do
  for slot in $(seq 1 "$slots"); do
    lock_file="$lock_dir/slot$slot.lock"
    exec {fd}>"$lock_file"
    if flock -n "$fd"; then
      # Record who holds it, so a stuck build is attributable. The lock is the
      # fd, not this file's contents -- a crashed holder releases it on exit.
      echo "pid=$$ wt=$(git rev-parse --show-toplevel 2>/dev/null) at=$(date -Is)" >&"$fd"
      waited=$(( $(date +%s) - start ))
      [ "$waited" -gt 5 ] && echo "$0: acquired build slot $slot after ${waited}s" >&2
      "$@"
      rc=$?
      flock -u "$fd"
      exec {fd}>&-
      exit "$rc"
    fi
    exec {fd}>&-
  done
  elapsed=$(( $(date +%s) - start ))
  if [ "$elapsed" -ge "$wait_sec" ]; then
    echo "$0: no build slot free after ${wait_sec}s; giving up" >&2
    exit 75
  fi
  # Never sleep past the deadline: a fixed poll longer than MBUN_BUILD_WAIT
  # would overshoot it and acquire a slot the caller had already given up on.
  remaining=$(( wait_sec - elapsed ))
  sleep "$(( remaining < 5 ? remaining : 5 ))"
done
