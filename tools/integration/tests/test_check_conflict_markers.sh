#!/usr/bin/env bash
# Self-test for check_conflict_markers.sh. The case that matters is the one the
# compiler cannot catch: a marker inside JS embedded in a C++ raw string.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
script="$repo_root/tools/integration/check_conflict_markers.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

repo="$tmp/repo"
mkdir -p "$repo/modules"
git -C "$repo" init -q
git -C "$repo" config user.email t@example.com
git -C "$repo" config user.name t

cat >"$repo/modules/clean.cppm" <<'EOF'
export module x;
const char* js = R"JS(
  function ok() { return 1; }
)JS";
EOF
# A doc file with a ======= banner must NOT trip the check.
printf 'Title\n=======\nbody\n' >"$repo/README.md"
git -C "$repo" add -A && git -C "$repo" commit -qm base

( cd "$repo" && bash "$script" >/dev/null 2>&1 ) \
  && pass "clean tree passes (a ======= banner is not a conflict)" \
  || fail "clean tree was reported dirty"

# The real case: markers inside JS in a raw string literal.
cat >"$repo/modules/dirty.cppm" <<'EOF'
export module y;
const char* js = R"JS(
  function execSync(cmd) {
<<<<<<< HEAD
    return a(cmd);
=======
    return b(cmd);
>>>>>>> other
  }
)JS";
EOF
git -C "$repo" add -A && git -C "$repo" commit -qm dirty

if ( cd "$repo" && bash "$script" >/dev/null 2>&1 ); then
  fail "marker inside an embedded-JS raw string was not detected"
fi
pass "marker inside embedded JS is detected"

out=$( cd "$repo" && bash "$script" 2>&1 || true )
printf '%s' "$out" | grep -q 'modules/dirty.cppm:4' \
  && pass "reports file:line" || fail "did not report file:line, got: $out"

# Path-scoped invocation only checks what it was given.
( cd "$repo" && bash "$script" modules/clean.cppm >/dev/null 2>&1 ) \
  && pass "path-scoped check ignores unrelated dirty files" \
  || fail "path-scoped check flagged an unrelated file"

# Untracked files are out of scope (git ls-files), so a scratch file with
# marker-like text cannot fail the build.
printf '<<<<<<< HEAD\n' >"$repo/scratch.txt"
( cd "$repo" && bash "$script" modules/clean.cppm >/dev/null 2>&1 ) \
  && pass "untracked scratch files are out of scope" \
  || fail "untracked file tripped the check"

echo "test_check_conflict_markers: ok"
