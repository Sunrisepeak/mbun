#!/usr/bin/env bash
# Self-test for safe-test.sh. The property that matters: a build-shaped command
# must get a memory limit big enough to compile, because an OOM-kill inside the
# scope surfaces as exit 124 -- the SAME code as a timeout. Two lanes each lost
# ~50 minutes to that ambiguity, one retrying at a two-hour bound.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/safe-test.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

cap() { set +e; bash "$tool" "$@" >"$tmp/o" 2>&1; local r=$?; set -e; return $r; }

# A build-shaped command raises the default and SAYS SO.
cap 10 mcpp --version || true
grep -q 'build-shaped command detected' "$tmp/o" \
  || fail "a build-shaped command must raise the limit and announce it"
grep -q 'MemoryMax=34G' "$tmp/o" || fail "the raised limit must be reported"
pass "a build-shaped command raises MemoryMax and announces it"

# A plain command must NOT be raised -- the tight limit is the freeze protection.
cap 10 /bin/true || true
grep -q 'build-shaped' "$tmp/o" && fail "a plain command must keep the 6G default"
pass "a plain command keeps the tight default"

# An explicit SAFE_MEM always wins over the detection.
set +e
SAFE_MEM=7G bash "$tool" 10 mcpp --version >"$tmp/o" 2>&1
set -e
grep -q 'build-shaped' "$tmp/o" && fail "an explicit SAFE_MEM must not be overridden"
pass "an explicit SAFE_MEM wins over detection"

# The wrapper still passes the command's own exit code through.
set +e
bash "$tool" 10 /bin/false >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "a failing command must not report success"
pass "the command exit code passes through"

# And a real timeout still yields 124, which is what the ambiguity was about.
set +e
bash "$tool" 1 sleep 30 >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 124 ] || fail "a timeout must still report 124, got $rc"
pass "a genuine timeout still reports 124"

printf '\nall safe-test self-tests passed\n'
