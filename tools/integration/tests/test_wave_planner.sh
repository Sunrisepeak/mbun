#!/usr/bin/env bash
# Self-test for wave_planner.py. The properties that matter: goals must follow
# MEASURED throughput, struck areas must never be planned, the resource guard must
# refuse rather than overcommit, and "100%" must be reported against what is
# actually reachable rather than implied.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/wave_planner.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# --- fixtures -----------------------------------------------------------------
mkdir -p "$tmp/noderun" "$tmp/bunrun"

# node run: test-alpha is rich and actionable, test-tls is rich but struck,
# test-tiny is below --min-actionable.
{
  printf 'path\tstatus\n'
  for i in $(seq 1 20); do printf 'compat/node/test/parallel/test-alpha-%s.js\tfail\n' "$i"; done
  for i in $(seq 1 30); do printf 'compat/node/test/parallel/test-alpha-g%s.js\tpass\n' "$i"; done
  for i in $(seq 1 15); do printf 'compat/node/test/parallel/test-tls-%s.js\tfail\n' "$i"; done
  for i in $(seq 1 3);  do printf 'compat/node/test/parallel/test-tiny-%s.js\tfail\n' "$i"; done
  for i in $(seq 1 9);  do printf 'compat/node/test/parallel/test-alpha-t%s.js\ttimeout\n' "$i"; done
} >"$tmp/noderun/results.tsv"

# bun run uses the bun runner's column layout (classification in column 7).
{
  printf 'path\texit_code\tpassed\tfailed\texpects\tran\tclassification\tduration_ms\tlog\n'
  for i in $(seq 1 12); do printf 'compat/bun/test/js/beta/x%s.test.ts\t1\t0\t1\t1\t1\ttest-failure\t10\tl\n' "$i"; done
  for i in $(seq 1 4);  do printf 'compat/bun/test/js/beta/g%s.test.ts\t0\t1\t0\t1\t1\tgreen\t10\tl\n' "$i"; done
  for i in $(seq 1 11); do printf 'compat/bun/test/napi/node-napi-tests/n%s.test.ts\t1\t0\t1\t1\t1\ttest-failure\t10\tl\n' "$i"; done
} >"$tmp/bunrun/results.tsv"

printf 'area\ttarget\tverdict\tcost_or_size\tevidence\n' >"$tmp/struck.tsv"
{
  printf 'node\ttls as a cluster target\tTOO_BIG\t62 fixable\thandshake engine deferred\n'
  printf 'bun\tnapi/node-napi-tests (napi addons)\tBLOCKED\t51 files\tonly 6 dynamic symbols exported\n'
} >>"$tmp/struck.tsv"

printf 'wave\tlane\tcorpus\tarea\tgoal\tdelivered\tminutes\tregressions\tnote\n' >"$tmp/ledger.tsv"
{
  printf '1\tA\tnode\ttest-alpha\t6\t10\t60\t0\t-\n'   # 10 files/hour
  printf '1\tB\tnode\ttest-beta\t6\t20\t60\t0\t-\n'   # 20 files/hour -> mean 15
  printf '1\tC\tbun\tjs/beta\t6\t2\t60\t0\t-\n'     # 2 files/hour
} >>"$tmp/ledger.tsv"

run() { python3 "$tool" --ledger "$tmp/ledger.tsv" --struck "$tmp/struck.tsv" "$@"; }
out() { run "$@" 2>&1 || true; }

# --- throughput ---------------------------------------------------------------
o=$(out --throughput)
echo "$o" | grep -q 'mean 15.0 files/hour' || fail "node mean should be 15.0 files/hour"
echo "$o" | grep -q 'best 20.0'            || fail "node best should be 20.0"
echo "$o" | grep -q 'mean 2.0 files/hour'  || fail "bun mean should be 2.0 files/hour"
pass "throughput is computed from the ledger, per corpus"

# --- goals follow measured throughput ----------------------------------------
o=$(out --plan 2 --node-run "$tmp/noderun" --bun-run "$tmp/bunrun")
echo "$o" | grep -q 'test-alpha' || fail "the rich actionable node area must be planned"
# node: mean 15/h x 2h = 30, capped by 20 actionable, x0.75 -> 15
echo "$o" | grep -qE 'GOAL \+15' || fail "node goal should be 15 (capped by 20 actionable)"
pass "a node goal is set from measured rate, capped by actionable count"

o=$(out --plan 5 --bun-run "$tmp/bunrun")
# bun: mean 2/h x 2h = 4, under the 12 actionable, x0.75 -> 3
echo "$o" | grep -qE 'GOAL \+3' || fail "bun goal should be 3 from its slower measured rate"
pass "a slower corpus gets a proportionally smaller goal"

# --- struck areas are never planned ------------------------------------------
o=$(out --plan 5 --node-run "$tmp/noderun" --bun-run "$tmp/bunrun")
echo "$o" | grep -q 'test-tls' && fail "test-tls is struck TOO_BIG and must never be planned"
pass "a struck node area is excluded even though its name differs from the entry"

echo "$o" | grep -q 'napi' && fail "napi is struck BLOCKED and must never be planned"
pass "a struck bun area is excluded"

# This is a regression test for a real miss: the first matcher compared whole
# strings, so area `test-tls` never matched target "tls as a cluster target" and
# tls was ranked FIRST in a live plan despite being struck.
python3 - "$tmp/struck.tsv" <<'PY' || fail "is_struck must match test-tls against 'tls as a cluster target'"
import sys, pathlib
sys.path.insert(0, str(pathlib.Path("tools/integration").resolve()))
import wave_planner as wp
ex = wp.struck_areas(wp.read_tsv(pathlib.Path(sys.argv[1])))
assert wp.is_struck("test-tls", ex), "test-tls should be struck"
assert wp.is_struck("napi/node-napi-tests", ex), "napi dir should be struck"
assert not wp.is_struck("test-alpha", ex), "test-alpha must NOT be struck"
assert not wp.is_struck("test-http2", ex), "test-http2 must NOT be struck"
PY
pass "is_struck matches on the area's core token at a word boundary"


# --- both corpora get lanes ---------------------------------------------------
o2=$(out --plan 4 --node-run "$tmp/noderun" --bun-run "$tmp/bunrun")
echo "$o2" | grep -q 'per-corpus split' || fail "a plan must state its per-corpus split"
echo "$o2" | grep -qE '\[bun\]'  || fail "the slower corpus must still get lanes (floor)"
echo "$o2" | grep -qE '\[node\]' || fail "the faster corpus must get lanes"
pass "the per-corpus floor keeps the slower corpus from being starved"

# --- thin areas are ignored ---------------------------------------------------
echo "$o" | grep -q 'test-tiny' && fail "an area under --min-actionable must be ignored"
pass "areas below --min-actionable are skipped"

# --- unverdicted files are not counted as actionable -------------------------
echo "$o" | grep -qE 'actionable 20 / no-verdict 9' \
  || fail "timeouts must be reported as no-verdict, never folded into actionable"
pass "timeout/oom/skip are excluded from actionable (that is how density gets overstated)"

# --- coverage names the unreachable remainder --------------------------------
o=$(out --coverage --node-run "$tmp/noderun" --bun-run "$tmp/bunrun")
echo "$o" | grep -q 'in struck/blocked areas  15' || fail "coverage must count struck failures separately"
echo "$o" | grep -q 'ceiling if every actionable file lands' \
  || fail "coverage must state the reachable ceiling, not just the current number"
echo "$o" | grep -q '100% is NOT reachable by lane work alone' \
  || fail "coverage must say plainly that 100% needs more than lanes"
pass "coverage separates green / actionable / struck / no-verdict and states the ceiling"

# --- resource guard -----------------------------------------------------------
o=$(out --plan 5 --node-run "$tmp/noderun")
echo "$o" | grep -q 'resource guard'   || fail "a plan must print its resource budget"
echo "$o" | grep -q 'disk:'            || fail "the guard must account for disk"
echo "$o" | grep -q 'mem:'             || fail "the guard must account for memory"
echo "$o" | grep -q 'campaign ceiling: 5 lanes' || fail "the 5-lane ceiling must be stated"
pass "the resource guard reports disk, memory, cpu and the campaign ceiling"

planned=$(echo "$o" | sed -n 's/.*=> planning \([0-9]*\) lane.*/\1/p')
[ -n "$planned" ] || fail "the guard must state how many lanes it will plan"
[ "$planned" -le 5 ] || fail "the guard must never exceed the campaign ceiling, got $planned"
pass "the planned count never exceeds the ceiling ($planned <= 5)"

# --- refusing to plan is an error, not a silent empty wave -------------------
set +e
DISK_OVERRIDE=1 python3 - <<'PY' >"$tmp/refuse.out" 2>&1
import pathlib, sys
sys.path.insert(0, str(pathlib.Path("tools/integration").resolve()))
import wave_planner as wp
# Simulate a nearly-full disk: reserve above what is free.
wp.DISK_GB_RESERVE = 10 ** 9
class A: plan=5; lane_hours=2.0; min_actionable=8
rc = wp.cmd_plan(A(), [("compat/node/test/parallel/test-alpha-1.js","fail")]*20, [], [], {})
print("rc=", rc)
PY
set -e
grep -q 'REFUSING to plan' "$tmp/refuse.out" || fail "an unaffordable wave must refuse explicitly"
grep -q 'rc= 1' "$tmp/refuse.out" || fail "refusing must be a non-zero exit, not a silent empty plan"
grep -q 'reclaim_disk' "$tmp/refuse.out" || fail "the refusal must name the remedy"
pass "an unaffordable wave refuses with exit 1 and names the remedy"

# --- mode selection is explicit ----------------------------------------------
set +e
python3 "$tool" --ledger "$tmp/ledger.tsv" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "no mode must be a usage error, not a silent no-op"
pass "a missing mode is a usage error"

# --- the checked-in inputs must parse ----------------------------------------
o=$(python3 "$tool" --throughput 2>&1)
echo "$o" | grep -q 'files/hour' || fail "the checked-in ledger must parse"
pass "the checked-in ledger parses"

printf '\nall wave_planner self-tests passed\n'
