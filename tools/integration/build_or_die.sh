#!/usr/bin/env bash
# build_or_die — build, refuse to continue if it failed, and print the binary.
#
# WHY THIS EXISTS: `bash build_lock.sh mcpp build 2>&1 | tail -2` throws away the
# exit code — a pipeline reports the status of its LAST command, so a failed
# build reads exactly like a successful one whose "Finished" line scrolled past.
# That is not hypothetical: a failed build was piped to `grep`, the stale binary
# from 48 minutes earlier was snapshotted as if fresh, and a full 4433-file
# corpus run was launched against it. The measurement would have been published
# as the merged tree's score while measuring code that predated two merges.
#
# So this wrapper makes the safe call the short one. On success it prints ONE
# line — the absolute path of the freshest mbun — so callers can do:
#
#     BIN=$(bash tools/integration/build_or_die.sh) || exit 1
#
# and cannot proceed with a stale binary, because there is nothing to proceed
# with. Anything else it has to say goes to stderr, keeping stdout parseable.
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root" || exit 1

log() { printf '%s\n' "$*" >&2; }

build_output=$(bash "$root/tools/integration/build_lock.sh" mcpp build 2>&1)
status=$?

if [ "$status" != 0 ]; then
  log "build_or_die: BUILD FAILED (exit $status) — refusing to hand back a binary."
  # The first few compiler errors are what a caller needs; the full log is long.
  printf '%s\n' "$build_output" | grep -E "error:|FAILED|failed" | head -12 >&2
  [ "$status" = 75 ] && log "build_or_die: exit 75 is build_lock giving up on the lock, not a compile error."
  exit "$status"
fi

binary=$(find "$root/target" -type f -name mbun -perm -111 -printf '%T@ %p\n' 2>/dev/null \
         | sort -rn | head -1 | cut -d' ' -f2-)
if [ -z "$binary" ]; then
  log "build_or_die: build reported success but no mbun binary exists under target/."
  exit 1
fi

# A successful no-op build is fine, but a binary older than the sources it should
# contain is not — that is the same stale-binary failure arriving by a different
# road (e.g. a build that succeeded in a DIFFERENT worktree while this one's
# configured state is broken).
newest_source=$(find "$root/modules" "$root/src" -type f \
                  \( -name '*.cppm' -o -name '*.cpp' -o -name '*.inc' -o -name '*.h' \) \
                  -newer "$binary" -print -quit 2>/dev/null)
if [ -n "$newest_source" ]; then
  log "build_or_die: STALE — $newest_source is newer than the binary it should be in."
  log "build_or_die: refusing. (A full reconfigure can silently leave a broken build"
  log "build_or_die:  state; deleting build.ninja has done exactly that here.)"
  exit 1
fi

log "build_or_die: ok — $(basename "$(dirname "$(dirname "$binary")")")"
printf '%s\n' "$binary"
