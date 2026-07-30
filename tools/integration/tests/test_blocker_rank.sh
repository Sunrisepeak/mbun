#!/usr/bin/env bash
# Self-test for blocker_rank.py. The properties that matter: a green file is
# never ranked, a file's blocker count is the number of DISTINCT causes, a test
# NAME is never mistaken for a cause (that bug made 32 files appear to share a
# blocker), and `sole` counts only files a fix would actually convert.
set -euo pipefail

tool="$(cd "$(dirname "$0")/.." && pwd)/blocker_rank.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

run="$tmp/run"; mkdir -p "$run/logs"

# bun schema: 9 columns, classification is column 7.
{
  printf 'path\texit_code\tpassed\tfailed\texpects\tran\tclassification\tduration_ms\tlog\n'
  printf 'compat/bun/test/a.test.ts\t1\t0\t1\t1\t1\ttest-failure\t10\tlogs/a.log\n'
  printf 'compat/bun/test/b.test.ts\t1\t0\t1\t1\t1\ttest-failure\t10\tlogs/b.log\n'
  printf 'compat/bun/test/c.test.ts\t1\t0\t2\t2\t2\ttest-failure\t10\tlogs/c.log\n'
  printf 'compat/bun/test/g.test.ts\t0\t1\t0\t1\t1\tgreen\t10\tlogs/g.log\n'
} > "$run/results.tsv"

# a and b share ONE cause -> each is a 1-blocker file, and the cause has sole=2.
printf 'bun test v1.3.14\n(fail) some test name\nTypeError: x is not a function\n' > "$run/logs/a.log"
printf 'bun test v1.3.14\n(fail) another name\nTypeError: x is not a function\n' > "$run/logs/b.log"
# c has TWO distinct causes -> not near-green, and neither cause gets sole credit.
printf 'TypeError: x is not a function\nRangeError: out of range\n' > "$run/logs/c.log"
printf 'all good\n' > "$run/logs/g.log"

o=$(python3 "$tool" --run "$run" --corpus bun --list-near)

# --- a green file is never ranked -------------------------------------------
echo "$o" | grep -q 'g.test.ts' && fail "a green file must never be ranked"
echo "$o" | grep -q '3 non-green files' || fail "must count exactly the 3 non-green files"
pass "green files are excluded"

# --- near-green is by DISTINCT cause count ----------------------------------
echo "$o" | grep -qE '2 file\(s\) at <= 1 blocker' \
  || fail "a and b are 1-blocker, c is 2-blocker: expected 2 near-green"
echo "$o" | grep -q 'a.test.ts' || fail "a must be listed as near-green"
echo "$o" | grep -q 'c.test.ts' && fail "c has two distinct causes and must NOT be near-green"
pass "blocker count is the number of distinct causes, not of failures"

# --- sole counts only files a fix converts ----------------------------------
# TypeError is mentioned by 3 files but is the SOLE blocker of only 2.
echo "$o" | grep -qE '2 sole  \(   3 mention\)' \
  || fail "TypeError must be sole=2 mention=3, got: $(echo "$o" | grep -i typeerror)"
pass "sole counts only the files a fix would convert, not all mentions"

# --- a test NAME is never a cause -------------------------------------------
# This is the bug the tool shipped with: `(fail) <name>` was parsed as a cause,
# so files "shared" a blocker that was really a common test name.
echo "$o" | grep -q 'some test name' && fail "a (fail) test name must never become a signature"
pass "a test name is never mistaken for a cause"

# --- targeted lookup lists the convertible files ----------------------------
o2=$(python3 "$tool" --run "$run" --corpus bun --signature 'TypeError: x is not a function')
echo "$o2" | grep -q 'a.test.ts' || fail "--signature must list the sole-blocked files"
echo "$o2" | grep -q 'c.test.ts' && fail "--signature must exclude multi-blocker files"
pass "--signature lists exactly the files a fix would convert"

# --- node schema is read too ------------------------------------------------
nrun="$tmp/nrun"; mkdir -p "$nrun/logs"
{
  printf 'path\texit_code\tclassification\tduration_ms\tlog\n'
  printf 'compat/node/test/parallel/test-x.js\t1\tfail\t10\tlogs/x.log\n'
  printf 'compat/node/test/parallel/test-y.js\t0\tpass\t10\tlogs/y.log\n'
} > "$nrun/results.tsv"
printf 'AssertionError: Expected values to be strictly equal:\n' > "$nrun/logs/x.log"
printf 'ok\n' > "$nrun/logs/y.log"
o3=$(python3 "$tool" --run "$nrun" --corpus node)
echo "$o3" | grep -q '1 non-green files' || fail "node schema: pass must be the green value"
pass "the node schema uses 'pass' as green"

# --- the caveat about assertion wrappers is always printed ------------------
echo "$o" | grep -q 'shared assertion FORM, not a shared cause' \
  || fail "must warn that an assertion wrapper is not a cause"
pass "the assertion-wrapper caveat is always printed"

printf '\nall blocker_rank self-tests passed\n'
