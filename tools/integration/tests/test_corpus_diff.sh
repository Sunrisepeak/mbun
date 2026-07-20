#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

diff_tool="$repo_root/tools/integration/corpus_diff.py"
columns="path	exit_code	passed	failed	expects	ran	classification	duration_ms	log"

# Two synthetic rounds sharing a schema with the runners' results.tsv. Between
# them: one file regresses (green -> test-failure), one file that is flaky and
# will be excused also regresses, one file gains (test-failure -> green), one
# file stays non-green but drops 40 passed assertions, one file stays non-green
# but its runtime doubles (perf move), and one green file is unchanged.
mkdir -p "$tmp/before" "$tmp/after"

{
  printf '%s\n' "$columns"
  printf 'stable.test.ts\t0\t5\t0\t5\t5\tgreen\t100\tlogs/a.log\n'
  printf 'regress.test.ts\t0\t3\t0\t3\t3\tgreen\t100\tlogs/b.log\n'
  printf 'flaky/net.test.ts\t0\t2\t0\t2\t2\tgreen\t100\tlogs/c.log\n'
  printf 'gain.test.ts\t1\t1\t1\t2\t2\ttest-failure\t100\tlogs/d.log\n'
  printf 'slip.test.ts\t1\t50\t2\t52\t52\ttest-failure\t100\tlogs/e.log\n'
  printf 'slow.test.ts\t1\t1\t1\t2\t2\ttest-failure\t200\tlogs/f.log\n'
} >"$tmp/before/results.tsv"

{
  printf '%s\n' "$columns"
  printf 'stable.test.ts\t0\t5\t0\t5\t5\tgreen\t100\tlogs/a.log\n'
  printf 'regress.test.ts\t1\t2\t1\t3\t3\ttest-failure\t100\tlogs/b.log\n'
  printf 'flaky/net.test.ts\t1\t0\t2\t2\t2\ttest-failure\t100\tlogs/c.log\n'
  printf 'gain.test.ts\t0\t2\t0\t2\t2\tgreen\t100\tlogs/d.log\n'
  printf 'slip.test.ts\t1\t10\t2\t12\t12\ttest-failure\t100\tlogs/e.log\n'
  printf 'slow.test.ts\t1\t1\t1\t2\t2\ttest-failure\t600\tlogs/f.log\n'
} >"$tmp/after/results.tsv"

# --- Bare diff: both regressions fail the gate (exit 1) ---------------------
set +e
report=$(python3 "$diff_tool" "$tmp/before" "$tmp/after" --top 10)
status=$?
set -e
test "$status" -eq 1
printf '%s\n' "$report" | grep -q 'REGRESSIONS (green -> non-green): 2'
printf '%s\n' "$report" | grep -q 'regress.test.ts'
printf '%s\n' "$report" | grep -q 'flaky/net.test.ts'
printf '%s\n' "$report" | grep -q 'gains (non-green -> green): 1'
printf '%s\n' "$report" | grep -q 'gain.test.ts'
printf '%s\n' "$report" | grep -q 'GATE: FAIL'
# The largest assertion move (slip: 50 -> 10) leads the moves list.
printf '%s\n' "$report" | grep -q -- '-40  slip.test.ts  (50 -> 10)'
# Per-bucket deltas: green 3 -> 2 (-1), test-failure 3 -> 4 (+1).
printf '%s\n' "$report" | grep -Eq 'green +3 +2 +-1'
printf '%s\n' "$report" | grep -Eq 'test-failure +3 +4 +\+1'

# --- Allow-manifest: excuse the flaky file, gate passes (exit 0) ------------
printf '%s\n' '# self-test allow manifest' 'flaky/*' >"$tmp/allow.txt"
set +e
report=$(python3 "$diff_tool" "$tmp/before" "$tmp/after" --allow-regressions "$tmp/allow.txt")
status=$?
set -e
test "$status" -eq 1  # regress.test.ts is still an un-excused regression
printf '%s\n' "$report" | grep -q 'REGRESSIONS (green -> non-green): 1'
printf '%s\n' "$report" | grep -q 'allowed regressions (excused by manifest): 1'

# Excuse BOTH regressions: now the gate passes and the exit code is 0.
printf '%s\n' 'flaky/*' 'regress.test.ts' >"$tmp/allow-all.txt"
set +e
report=$(python3 "$diff_tool" "$tmp/before" "$tmp/after" --allow-regressions "$tmp/allow-all.txt")
status=$?
set -e
test "$status" -eq 0
printf '%s\n' "$report" | grep -q 'REGRESSIONS (green -> non-green): 0'
printf '%s\n' "$report" | grep -q 'GATE: PASS -- no un-excused regressions (2 excused)'

# --- JSON summary mirrors the report and is machine-parseable ---------------
python3 "$diff_tool" "$tmp/before" "$tmp/after" --json >"$tmp/out.json" || true
python3 - "$tmp/out.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["gate_failed"] is True, d["gate_failed"]
assert sorted(d["regressions"]) == ["flaky/net.test.ts", "regress.test.ts"], d["regressions"]
assert d["gains"] == ["gain.test.ts"], d["gains"]
assert d["buckets"]["green"] == {"before": 3, "after": 2, "delta": -1}, d["buckets"]["green"]
assert d["buckets"]["test-failure"] == {"before": 3, "after": 4, "delta": 1}, d["buckets"]["test-failure"]
# Largest assertion move first.
assert d["moves"][0] == {"path": "slip.test.ts", "before_passed": 50,
                         "after_passed": 10, "delta": -40}, d["moves"][0]
assert d["added"] == [] and d["dropped"] == []
PY

# --- Perf mode: slow.test.ts (200ms -> 600ms, x3) is flagged ----------------
set +e
report=$(python3 "$diff_tool" "$tmp/before" "$tmp/after" --perf --perf-threshold 1.5)
set -e
printf '%s\n' "$report" | grep -q 'perf regressions'
printf '%s\n' "$report" | grep -q -- 'x3.00  slow.test.ts  (200ms -> 600ms)'
# Below the 100ms floor a doubling must NOT be flagged: re-run with a huge floor.
set +e
report=$(python3 "$diff_tool" "$tmp/before" "$tmp/after" --perf --perf-floor-ms 10000)
set -e
! printf '%s\n' "$report" | grep -q 'perf regressions'

# --- Added / dropped files are reported, not miscounted as regressions ------
{ cat "$tmp/before/results.tsv"; printf 'gone.test.ts\t0\t1\t0\t1\t1\tgreen\t100\tlogs/g.log\n'; } >"$tmp/before2.tsv"
mkdir -p "$tmp/before2"; mv "$tmp/before2.tsv" "$tmp/before2/results.tsv"
set +e
report=$(python3 "$diff_tool" "$tmp/before2" "$tmp/after")
set -e
# A green file that vanished is a dropped file, not a green->non-green regression.
printf '%s\n' "$report" | grep -q 'dropped: 1'
printf '%s\n' "$report" | grep -q -- '- gone.test.ts'
printf '%s\n' "$report" | grep -q 'REGRESSIONS (green -> non-green): 2'

# --- A malformed / missing results.tsv fails loudly (not a silent pass) ------
set +e
python3 "$diff_tool" "$tmp/before" "$tmp/nonexistent" >/dev/null 2>&1
status=$?
set -e
test "$status" -ne 0

echo "test_corpus_diff: ok"
