#!/usr/bin/env bash
# Self-test for check_submodule_gitlinks.sh.
#
# The damage it guards is invisible to a build and invisible on the machine that
# causes it: every agent worktree wires compat/{bun,node} as symlinks, and a
# symlink staged in place of a submodule gitlink still resolves locally. It
# reached a merge commit once, announced only by a `mode change 160000 => 120000`
# line between two source files.
#
# Driven against a throwaway repository, so it exercises the real git plumbing
# rather than a mocked ls-tree.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/check_submodule_gitlinks.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

cd "$tmp"
git init -q .
git config user.email t@t.t
git config user.name t
mkdir -p compat
echo x >seed
git add seed
git commit -qm seed
# Any commit-ish works as a gitlink target; the guard checks the MODE, not that
# the submodule is checked out (it never is, in a linked worktree).
sha=$(git rev-parse HEAD)
git update-index --add --cacheinfo 160000,"$sha",compat/bun
git update-index --add --cacheinfo 160000,"$sha",compat/node

"$tool" >/dev/null 2>&1 || fail "a correct index with two gitlinks was rejected"
pass "two 160000 gitlinks in the index are accepted"

# --- the real failure: a worktree symlink staged over the gitlink ------------
ln -s /somewhere/else/bun "$tmp/link"
blob=$(git hash-object -w --stdin <<<"/somewhere/else/bun")
git update-index --add --cacheinfo 120000,"$blob",compat/bun

if "$tool" >/dev/null 2>&1; then
  fail "a symlink staged over compat/bun was accepted"
fi
pass "a 120000 symlink in place of a gitlink is rejected"

out=$("$tool" 2>&1 || true)
grep -q "compat/bun has mode 120000" <<<"$out" || fail "did not name the offending path and mode: $out"
grep -q "update-index --cacheinfo 160000" <<<"$out" || fail "did not print the repair command: $out"
pass "names the path, the mode, and the repair command"

# compat/node is still correct and must not be reported
grep -q "compat/node has mode" <<<"$out" && fail "reported compat/node, which is fine: $out"
pass "reports only the path that is actually wrong"

# --- a tree argument is checked, not just the index -------------------------
git update-index --add --cacheinfo 160000,"$sha",compat/bun   # repair the index
git commit -qm good
good=$(git rev-parse HEAD)
git update-index --add --cacheinfo 120000,"$blob",compat/node
git commit -qm bad
bad=$(git rev-parse HEAD)

"$tool" "$good" >/dev/null 2>&1 || fail "a good commit was rejected"
pass "a good commit passes when named as a revision"
if "$tool" "$bad" >/dev/null 2>&1; then
  fail "a commit with a symlinked submodule was accepted"
fi
pass "a bad commit is caught when named as a revision"

# --- an absent path is a failure, not a silent pass --------------------------
git rm -q --cached compat/node >/dev/null
if "$tool" >/dev/null 2>&1; then
  fail "an index missing compat/node was accepted"
fi
pass "a missing submodule path fails rather than passing silently"

echo "test_check_submodule_gitlinks: ok"
