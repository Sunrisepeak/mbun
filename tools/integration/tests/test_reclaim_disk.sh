#!/usr/bin/env bash
# Self-test for reclaim_disk.sh. The behaviour that matters is what it REFUSES
# to delete: the first cut of this script would have wiped the build caches of
# five worktrees that were mid-wave. So every assertion here is about a keep.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
script="$repo_root/tools/integration/reclaim_disk.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

main="$tmp/main"
mkdir -p "$main"
git -C "$main" init -q
git -C "$main" config user.email t@example.com
git -C "$main" config user.name t
echo hi >"$main/f"
git -C "$main" add f
git -C "$main" commit -qm base
git -C "$main" worktree add -q --detach "$tmp/stale" HEAD
git -C "$main" worktree add -q --detach "$tmp/active" HEAD

# Build caches in both linked worktrees, at both nesting depths the real repo
# uses (`<wt>/target` and `<wt>/modules/<member>/target`).
for wt in "$tmp/stale" "$tmp/active"; do
  mkdir -p "$wt/target/x" "$wt/modules/jsc/target/y"
  head -c 200000 /dev/zero >"$wt/target/x/blob"
  head -c 200000 /dev/zero >"$wt/modules/jsc/target/y/blob"
  echo "source" >"$wt/modules/jsc/keep.cppm"
done
# Age the stale one well past the default 24h window.
find "$tmp/stale" -exec touch -d '5 days ago' {} +

# --- dry run must not delete anything ----------------------------------------
out=$( cd "$main" && bash "$script" 2>&1 )
[ -e "$tmp/stale/target/x/blob" ] \
  && pass "dry run deletes nothing" || fail "dry run deleted a cache"
printf '%s' "$out" | grep -q 'Re-run with --apply' \
  && pass "dry run says how to apply" || fail "dry run does not mention --apply"

# --- the active worktree is protected by mtime, the stale one is not ----------
printf '%s' "$out" | grep -q "keep .*$tmp/active" \
  && pass "recently-built worktree is kept" \
  || fail "would have reclaimed an in-use worktree: $out"
printf '%s' "$out" | grep -q "would .*$tmp/stale" \
  && pass "stale worktree is offered" || fail "stale worktree not offered: $out"

# --- --min-gb short-circuits when there is plenty of room --------------------
out2=$( cd "$main" && bash "$script" --apply --min-gb 1 2>&1 )
printf '%s' "$out2" | grep -q 'nothing to do' \
  && pass "--min-gb skips when free space is above the floor" \
  || fail "--min-gb did not short-circuit: $out2"
[ -e "$tmp/stale/target/x/blob" ] \
  && pass "--min-gb skip deleted nothing" || fail "deleted despite --min-gb skip"

# --- apply removes stale caches and ONLY caches -------------------------------
out3=$( cd "$main" && bash "$script" --apply 2>&1 )
[ ! -e "$tmp/stale/target" ] \
  && pass "stale cache removed" || fail "stale cache survived --apply"
[ ! -e "$tmp/stale/modules/jsc/target" ] \
  && pass "nested member cache removed" || fail "nested cache survived"
[ -f "$tmp/stale/modules/jsc/keep.cppm" ] \
  && pass "source files next to a cache are untouched" || fail "deleted a source file"
[ -f "$tmp/stale/f" ] \
  && pass "tracked files are untouched" || fail "deleted a tracked file"
[ -e "$tmp/active/target/x/blob" ] \
  && pass "in-use worktree survived --apply" || fail "APPLY DELETED AN IN-USE CACHE"
printf '%s' "$out3" | grep -q 'freed' \
  && pass "apply reports what it freed" || fail "apply did not report: $out3"

# --- the current worktree is never a candidate, even when old ----------------
mkdir -p "$main/target/z"
touch -d '5 days ago' "$main/target/z" "$main/target"
out4=$( cd "$main" && bash "$script" 2>&1 )
printf '%s' "$out4" | grep -q "keep .*$main (active)" \
  && pass "current checkout is kept even when its cache is old" \
  || fail "offered the current checkout: $out4"

# --- --keep protects an otherwise-stale worktree ------------------------------
git -C "$main" worktree add -q --detach "$tmp/spare" HEAD
mkdir -p "$tmp/spare/target"
head -c 100000 /dev/zero >"$tmp/spare/target/blob"
find "$tmp/spare" -exec touch -d '5 days ago' {} +
out5=$( cd "$main" && bash "$script" --keep "$tmp/spare" 2>&1 )
printf '%s' "$out5" | grep -q "keep .*$tmp/spare" \
  && pass "--keep protects a stale worktree" || fail "--keep ignored: $out5"

# --- --stale-hours 0 disables the mtime guard entirely ------------------------
out6=$( cd "$main" && bash "$script" --stale-hours 0 2>&1 )
printf '%s' "$out6" | grep -q "would .*$tmp/active" \
  && pass "--stale-hours 0 offers even a fresh cache" \
  || fail "--stale-hours 0 still filtered: $out6"

echo "test_reclaim_disk: ok"
