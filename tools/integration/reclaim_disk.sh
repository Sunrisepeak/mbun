#!/usr/bin/env bash
# reclaim_disk.sh -- reclaim regenerable build caches from STALE agent worktrees.
#
# WHY: the bounded layer already refuses to start a measurement when the disk is
# nearly full (`bounded_run.ensure_disk_headroom()`). That guard is correct, but
# on 2026-07-29 it was one step from firing across the whole campaign: the box
# was at 100% with 6.1 GB free on a 1.5 TB disk, and every corpus run would have
# aborted with an error that reads like a runner bug rather than like a full
# disk.
#
# The cause is structural, not accidental. Every parallel-agent round leaves
# behind worktrees, and each worktree accumulates a `target/` tree PLUS a
# per-member `modules/*/target/` tree. They are never cleaned because no single
# agent owns them: `.claude/worktrees/wt4` alone held 11 GB under
# `modules/jsc/target`, and five stale round-2/round-9 worktrees held 49 GB
# between them. Reclaiming just those caches took the box from 6.1 GB to 44 GB
# free without touching a single line of source.
#
# What this deletes: `target/` directories inside worktrees you name. Those are
# 100% regenerable build artifacts.
#
# STALENESS IS BY MTIME, NOT BY "is it the current worktree". The first cut of
# this script protected only the current checkout, and its dry run promptly
# offered to delete the five worktrees of the wave then executing -- which would
# have handed every running lane a cold rebuild mid-timebox, in the name of
# freeing space that was no longer scarce. A worktree whose cache was written
# recently is by definition in use by somebody, and no single agent can see who.
# So a cache is reclaimable only if nothing has touched it for --stale-hours.
#
# What this NEVER touches, and why:
#   - the worktree's tracked or untracked SOURCE files -- uncommitted agent work
#     lives there, and several stale worktrees still carry dirty trees;
#   - any cache modified within --stale-hours -- another agent is building on it;
#   - the CURRENT worktree's `target/` -- that is the incremental cache the next
#     build needs; blowing it away trades disk for a cold rebuild;
#   - the shared `~/.mcpp/bmi` module cache -- it is shared by EVERY worktree on
#     the machine, so clearing it gives every other running agent a cold rebuild
#     (this is the same trap documented for `mcpp clean --bmi-cache`).
#
# Usage:
#   tools/integration/reclaim_disk.sh                  # dry run: report only
#   tools/integration/reclaim_disk.sh --apply          # delete stale caches
#   tools/integration/reclaim_disk.sh --apply --min-gb 20
#
# Options:
#   --apply            actually delete; without it nothing is removed
#   --min-gb <n>       only act when free space is below <n> GB (default 0 = always)
#   --stale-hours <n>  a cache must be untouched this long to count as stale
#                      (default 24; use 0 only when you know the box is idle)
#   --keep <path>      additionally protect a worktree (repeatable)
#   --prune-configs    ALSO prune superseded per-config build dirs everywhere,
#                      including the CURRENT worktree (see below)
#
# --prune-configs: the second, larger leak
#
# The worktree pass above cannot touch the current checkout, and that is correct
# for its incremental cache -- but it means the main checkout's build output grows
# without bound in a way nobody notices. mcpp keys build output by config hash:
# `<root>/target/<arch>/<hash>/` and `<root>/modules/<member>/target/<arch>/<hash>/`.
# Every toolchain or config change mints a NEW hash and abandons the old tree,
# fully populated. Measured on 2026-07-30: `modules/jsc/target` alone held 39 GB
# across four hashes, three of them 10-12 days old and 9.4 GB each, left behind by
# a gcc->llvm->gcc toolchain flip. The box was at 100% with 9.1 GB free and lanes
# were about to start failing; pruning superseded configs freed 51 GB and the very
# next `mcpp build` was a 0.06s no-op, i.e. nothing live was touched.
#
# What counts as superseded, per `<root>/target/<arch>/` directory:
#   - the NEWEST hash dir is the live config -- always kept;
#   - anything touched within --stale-hours is kept (another lane is building);
#   - everything else is an abandoned config and is deleted.
# This is safe in the current worktree in a way deleting `target/` wholesale is
# not: the live incremental cache survives, so no cold rebuild is forced.
#
# THAT SAID, RECLAIM THE IDLE WORKTREES AGGRESSIVELY. The instinct to preserve a
# lane worktree's live cache is wrong on this box: a lane measured a COLD build at
# **95 seconds**. Four idle lane worktrees were holding ~59 GB of live jsc build
# output between them to save ~95s each -- a bad trade at any disk pressure, and
# an actively dangerous one at 99% full, where the bounded layer starts refusing
# measurements and the failure reads like a runner bug. Deleting
# `<wt>/target`, `<wt>/modules/*/target` and `<wt>/.mcpp` for every worktree with
# no running process took this box from 22 GB to 103 GB free in one pass. Sources
# are never touched, so no uncommitted lane work is at risk.
#
# Exit 0 on success, 2 on usage error.
set -uo pipefail

apply=0
min_gb=0
stale_hours=24
prune_configs=0
keeps=()

while [ $# -gt 0 ]; do
  case "$1" in
    --apply)       apply=1; shift ;;
    --min-gb)      min_gb="${2:?--min-gb needs a value}"; shift 2 ;;
    --stale-hours) stale_hours="${2:?--stale-hours needs a value}"; shift 2 ;;
    --keep)        keeps+=("${2:?--keep needs a path}"); shift 2 ;;
    --prune-configs) prune_configs=1; shift ;;
    -h|--help) sed -n '2,74p' "$0"; exit 0 ;;
    *) echo "$0: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

common_dir=$(git rev-parse --git-common-dir 2>/dev/null) || {
  echo "$0: not in a git repository" >&2; exit 2; }
main_checkout=$(cd "$(dirname "$common_dir")" && pwd)
current_wt=$(git rev-parse --show-toplevel)

free_gb() { df -BG --output=avail / | tail -1 | tr -dc '0-9'; }

before=$(free_gb)
if [ "$min_gb" -gt 0 ] && [ "$before" -ge "$min_gb" ]; then
  echo "reclaim_disk: ${before}G free, above --min-gb ${min_gb}G; nothing to do"
  exit 0
fi

# Protect the current worktree and the main checkout unconditionally: their
# incremental caches are what the next build depends on.
protected=("$current_wt" "$main_checkout" "${keeps[@]+"${keeps[@]}"}")
is_protected() {
  local candidate="$1" p
  for p in "${protected[@]}"; do
    [ "$candidate" = "$p" ] && return 0
  done
  return 1
}

# `git worktree list --porcelain` is the authority on which worktrees exist --
# scanning directories by name would miss re-pointed ones and would happily
# delete a directory git no longer tracks.
mapfile -t worktrees < <(git worktree list --porcelain | awk '/^worktree /{print $2}')

total_freed=0
for wt in "${worktrees[@]}"; do
  if is_protected "$wt"; then
    echo "keep    $wt (active)"
    continue
  fi
  [ -d "$wt" ] || continue
  # -maxdepth 3 reaches both `<wt>/target` and `<wt>/modules/<member>/target`
  # without descending into the caches themselves.
  mapfile -t all_caches < <(find "$wt" -maxdepth 3 -type d -name target -prune 2>/dev/null)
  [ "${#all_caches[@]}" -gt 0 ] || continue

  # Keep only caches nothing has touched for --stale-hours. `-print -quit` stops
  # at the FIRST recent file, so this stays cheap even over a 9 GB tree.
  caches=()
  recent=0
  for c in "${all_caches[@]}"; do
    if [ "$stale_hours" -gt 0 ] &&
       [ -n "$(find "$c" -newermt "-${stale_hours} hours" -print -quit 2>/dev/null)" ]; then
      recent=$((recent + 1))
      continue
    fi
    caches+=("$c")
  done
  if [ "$recent" -gt 0 ] && [ "${#caches[@]}" -eq 0 ]; then
    echo "keep    $wt (built within ${stale_hours}h -- in use)"
    continue
  fi
  [ "${#caches[@]}" -gt 0 ] || continue
  size=$(du -sm "${caches[@]}" 2>/dev/null | awk '{s+=$1} END {print s+0}')
  [ "$size" -gt 0 ] || continue
  total_freed=$((total_freed + size))
  if [ "$apply" -eq 1 ]; then
    rm -rf "${caches[@]}"
    echo "reclaim $wt  ${size}M  (${#caches[@]} cache dirs)"
  else
    echo "would   $wt  ${size}M  (${#caches[@]} cache dirs)"
  fi
done

# Second pass: superseded per-config build dirs. Unlike the worktree pass this is
# safe in the CURRENT worktree, because the newest hash -- the live incremental
# cache -- is always kept.
if [ "$prune_configs" -eq 1 ]; then
  while IFS= read -r arch_dir; do
    newest=$(ls -1dt "$arch_dir"/*/ 2>/dev/null | head -1)
    [ -n "$newest" ] || continue
    for hash_dir in "$arch_dir"/*/; do
      [ -d "$hash_dir" ] || continue
      # The live config: whatever the next build will reuse.
      [ "$hash_dir" = "$newest" ] && { echo "keep    $hash_dir (live config)"; continue; }
      # Somebody is mid-build against this one.
      if [ -n "$(find "$hash_dir" -maxdepth 3 -newermt "-${stale_hours} hours" -print -quit 2>/dev/null)" ]; then
        echo "keep    $hash_dir (touched within ${stale_hours}h)"
        continue
      fi
      size=$(du -sm "$hash_dir" 2>/dev/null | awk '{print $1+0}')
      [ "${size:-0}" -gt 0 ] || continue
      total_freed=$((total_freed + size))
      if [ "$apply" -eq 1 ]; then
        rm -rf "$hash_dir"; echo "reclaim $hash_dir  ${size}M  (superseded config)"
      else
        echo "would   $hash_dir  ${size}M  (superseded config)"
      fi
    done
  done < <(find . -mindepth 2 -maxdepth 4 -type d -path '*/target/*' \
                  \( -name 'x86_64-*' -o -name 'aarch64-*' \) 2>/dev/null)
fi

after=$(free_gb)
if [ "$apply" -eq 1 ]; then
  echo "reclaim_disk: freed ~${total_freed}M; ${before}G -> ${after}G free"
else
  echo "reclaim_disk: ~${total_freed}M reclaimable; ${before}G free. Re-run with --apply."
fi
