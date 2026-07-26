#!/usr/bin/env bash
# Self-test for worktree_setup.sh. Builds a throwaway repo with the same shape
# as mbun (compat/{bun,node} populated in the main checkout) and exercises the
# failure mode the script exists to prevent: a fresh worktree leaves an EMPTY
# compat/node directory, and a naive `ln -sfn` links INSIDE it.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
script="$repo_root/tools/integration/worktree_setup.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

main="$tmp/main"
mkdir -p "$main"
git -C "$main" init -q
git -C "$main" config user.email t@example.com
git -C "$main" config user.name t
mkdir -p "$main/compat/node/test/parallel" "$main/compat/node/test/common" "$main/compat/bun/test"
echo "// t" >"$main/compat/node/test/parallel/test-a.js"
echo "// t" >"$main/compat/node/test/parallel/test-b.js"
echo "// common" >"$main/compat/node/test/common/index.js"
echo "// bun" >"$main/compat/bun/test/x.test.ts"
# compat/ is untracked here, exactly as the real submodules are to a worktree.
printf 'compat/\n' >"$main/.gitignore"
echo hi >"$main/file.txt"
git -C "$main" add .gitignore file.txt
git -C "$main" commit -qm "base"
base=$(git -C "$main" rev-parse HEAD)
echo more >>"$main/file.txt"
git -C "$main" commit -qam "second"

# --- fresh worktree ---------------------------------------------------------
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/x)
[ -L "$tmp/wt1/compat/node" ] && [ -L "$tmp/wt1/compat/bun" ] \
  && pass "fresh worktree gets compat symlinks" || fail "compat not symlinked: $out"
[ -f "$tmp/wt1/compat/node/test/parallel/test-a.js" ] \
  && pass "corpus path resolves through the symlink" || fail "corpus path broken"
[ "$(git -C "$tmp/wt1" branch --show-current)" = "feature/x" ] \
  && pass "branch created" || fail "wrong branch"

# --- the actual trap: an empty compat/node directory must be replaced --------
rm "$tmp/wt1/compat/node"
mkdir -p "$tmp/wt1/compat/node"
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/x)
[ -L "$tmp/wt1/compat/node" ] \
  && pass "empty compat/node directory replaced by a symlink" || fail "empty dir not replaced"
[ ! -e "$tmp/wt1/compat/node/node" ] \
  && pass "link did not land inside the directory" || fail "created compat/node/node — the bug reproduced"

# --- repairs a previous run's inside-the-directory mistake -------------------
rm "$tmp/wt1/compat/node"
mkdir -p "$tmp/wt1/compat/node"
ln -s "$main/compat/node" "$tmp/wt1/compat/node/node"
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/x)
[ -L "$tmp/wt1/compat/node" ] && [ -f "$tmp/wt1/compat/node/test/parallel/test-a.js" ] \
  && pass "repairs a previously mis-wired worktree" || fail "did not repair mis-wiring"

# --- re-pointing keeps target/ (the expensive build cache) ------------------
mkdir -p "$tmp/wt1/target/x"; echo cached >"$tmp/wt1/target/x/artifact"
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/y "$base")
[ -f "$tmp/wt1/target/x/artifact" ] \
  && pass "re-point preserves target/ build cache" || fail "target/ was cleaned"
[ "$(git -C "$tmp/wt1" rev-parse HEAD)" = "$base" ] \
  && pass "re-point honours the start-point argument" || fail "wrong start point"
[ "$(git -C "$tmp/wt1" branch --show-current)" = "feature/y" ] \
  && pass "re-point switches branch" || fail "branch not switched"

# --- the generated build graph is dropped, object files are kept ------------
# A stale build.ninja does not know about a module partition added by the new
# start-point and makes mcpp build fail with a misleading "imports must be built
# before being imported". Re-pointing must clear it without nuking the cache.
mkdir -p "$tmp/wt1/target/abc"
echo stale >"$tmp/wt1/target/abc/build.ninja"
echo cached >"$tmp/wt1/target/abc/object.o"
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/y "$base")
[ ! -e "$tmp/wt1/target/abc/build.ninja" ] \
  && pass "stale build.ninja removed on re-point" || fail "build.ninja survived"
[ -f "$tmp/wt1/target/abc/object.o" ] \
  && pass "object files kept (incremental cache survives)" || fail "object files were deleted"

# --- a populated non-submodule directory is never destroyed -----------------
rm "$tmp/wt1/compat/bun"
mkdir -p "$tmp/wt1/compat/bun/precious"; echo keep >"$tmp/wt1/compat/bun/precious/data"
out=$(cd "$main" && bash "$script" "$tmp/wt1" feature/y 2>&1 || true)
[ -f "$tmp/wt1/compat/bun/precious/data" ] \
  && pass "populated compat dir left intact" || fail "destroyed a populated compat dir"

# --- a bad start-point fails loudly ----------------------------------------
if (cd "$main" && bash "$script" "$tmp/wt2" feature/z no-such-ref >/dev/null 2>&1); then
  fail "a nonexistent start-point should exit non-zero"
fi
pass "bad start-point rejected"

echo "test_worktree_setup: ok"
