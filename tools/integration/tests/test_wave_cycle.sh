#!/usr/bin/env bash
# Self-test for wave_cycle.py. The properties that matter: goal accuracy must be
# computed ONLY over lanes that carried a goal (mixing in goal-free lanes made
# wave 65 read 4.50 when its one goal-bearing lane delivered 0 of 6), the mode
# must flip on the measured near-green fraction, a mode read off a SUBSET run
# must say so, and a wave with no ledger rows must fail loudly.
set -euo pipefail

tool="$(cd "$(dirname "$0")/.." && pwd)/wave_cycle.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# --- goal accuracy ignores goal-free lanes ----------------------------------
led="$tmp/ledger.tsv"
{
  printf 'wave\tlane\tcorpus\tarea\tshape\tgoal\tdelivered\tminutes\tregressions\tnote\n'
  printf '99\tA\tnode\talpha\tfix\t10\t5\t60\t0\thalf of a real goal\n'
  printf '99\tB\tbun\tbeta\tmeasure\t0\t20\t60\t0\tgoal-free: census/re-audit\n'
} > "$led"
o=$(WAVE_CYCLE_LEDGER="$led" python3 - "$tool" "$led" <<'EOF'
import runpy, sys, pathlib
tool, led = sys.argv[1], sys.argv[2]
import importlib.util
spec = importlib.util.spec_from_file_location("wc", tool)
wc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wc)
wc.LEDGER = pathlib.Path(led)
ev = wc.evaluate("99")
print("acc", ev["goal_accuracy"], "goaled", ev["goaled_lanes"],
      "unplanned", ev["unplanned_delivered"], "total", ev["total_delivered"])
EOF
)
echo "$o" | grep -q 'acc 0.5 goaled 1' \
  || fail "accuracy must be 5/10 over the ONE goal-bearing lane, got: $o"
echo "$o" | grep -q 'unplanned 20.0' \
  || fail "the goal-free lane's 20 files must be reported as unplanned, got: $o"
echo "$o" | grep -q 'total 25.0' || fail "total must still count everything, got: $o"
pass "goal accuracy covers only goal-bearing lanes; unplanned work is separated"

# --- the mode flips on the measured fraction --------------------------------
o=$(python3 - "$tool" <<'EOF'
import sys, importlib.util
spec = importlib.util.spec_from_file_location("wc", sys.argv[1])
wc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wc)
print("node", wc.mode_for(190, 374)[0])   # 51% -> file-by-file pays
print("bun",  wc.mode_for(78, 924)[0])    # 8%  -> it does not
print("edge", wc.mode_for(20, 100)[0])    # exactly at the threshold
EOF
)
echo "$o" | grep -q 'node AREA'       || fail "190/374 (51%) must be AREA: $o"
echo "$o" | grep -q 'bun STRUCTURAL'  || fail "78/924 (8%) must be STRUCTURAL: $o"
echo "$o" | grep -q 'edge AREA'       || fail "the threshold must be inclusive: $o"
pass "mode flips on the measured near-green fraction, threshold inclusive"

# --- a subset run is flagged, a full one is not -----------------------------
o=$(python3 - "$tool" <<'EOF'
import sys, importlib.util, pathlib, tempfile
spec = importlib.util.spec_from_file_location("wc", sys.argv[1])
wc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wc)
d = pathlib.Path(tempfile.mkdtemp())
(d / "results.tsv").write_text("path\tclassification\n" + "x\tpass\n" * 500)
print("size", wc.run_size(d), "full_bun", wc.CORPUS_SIZE["bun"])
EOF
)
echo "$o" | grep -q 'size 500 full_bun 1902' \
  || fail "run_size must count data rows and know the corpus size: $o"
pass "a subset run is measurable against the full corpus size"

# --- an unknown wave fails loudly, not silently ------------------------------
set +e
python3 - "$tool" "$led" >/dev/null 2>&1 <<'EOF'
import sys, importlib.util, pathlib
spec = importlib.util.spec_from_file_location("wc", sys.argv[1])
wc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wc)
wc.LEDGER = pathlib.Path(sys.argv[2])
wc.evaluate("does-not-exist")
EOF
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "evaluating an unknown wave must fail, not return empty"
pass "an unknown wave fails loudly"

# --- per-area rates are computed per AREA, not pooled ------------------------
# This is what the corpus mean hides: two lanes, same goal, same near-green
# axis, 7.5x apart because one area's blockers shared a cause and the other's
# did not. Pooling them makes both goals wrong.
o=$(python3 - "$tool" "$tmp/ar.tsv" <<'EOF'
import sys, importlib.util, pathlib
spec = importlib.util.spec_from_file_location("wc", sys.argv[1])
wc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wc)
rows = [
  {"area": "fast", "delivered": "6", "minutes": "60"},    # 6.0/h
  {"area": "slow", "delivered": "2", "minutes": "156"},   # 0.77/h
  {"area": "slow", "delivered": "4", "minutes": "204"},   # pooled with the above
  {"area": "nohours", "delivered": "9", "minutes": "0"},  # unmeasurable, must drop
]
r = wc.area_rates(rows)
print("fast", r["fast"]["rate"], "lanes", r["fast"]["lanes"])
print("slow", r["slow"]["rate"], "lanes", r["slow"]["lanes"])
print("nohours_present", "nohours" in r)
EOF
)
echo "$o" | grep -q 'fast 6.0 lanes 1'  || fail "6 files in 60min must be 6.0/h: $o"
echo "$o" | grep -q 'slow 1.0 lanes 2'  || fail "slow must pool its 2 lanes to 6 files/6h = 1.0/h: $o"
echo "$o" | grep -q 'nohours_present False'   || fail "a zero-minute row is unmeasurable and must not produce a rate: $o"
pass "rates are per-area, pooled within an area, and zero-minute rows are dropped"

# --- the checked-in ledger evaluates ----------------------------------------
python3 "$tool" --evaluate 65 >/dev/null 2>&1 || fail "the checked-in ledger must evaluate"
pass "the checked-in ledger evaluates"

printf '\nall wave_cycle self-tests passed\n'
