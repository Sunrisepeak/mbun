#!/usr/bin/env bash
# Self-test for latency_probe.py, driven by a fake runtime whose delay we control.
# The property that matters: an order-of-magnitude regression must FAIL the gate,
# and normal variance must NOT.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/latency_probe.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# Fake mbun: sleeps FAKE_DELAY seconds for any script with a body, and returns
# immediately for the empty startup probe (so the measured floor stays near 0).
cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
if [ -s "$1" ]; then sleep "${FAKE_DELAY:-0}"; fi
exit "${FAKE_EXIT:-0}"
EOF
chmod +x "$tmp/fake-mbun"

run() { PYTHONPATH="$repo_root/tools/integration" python3 "$tool" --bin "$tmp/fake-mbun" \
          --out "$tmp/out" --repeats 1 --timeout 10 "$@"; }

# --- fast runtime passes its thresholds -------------------------------------
FAKE_DELAY=0 run --only dns.lookup-literal >/dev/null 2>&1 \
  && pass "a fast probe passes its threshold" || fail "fast probe failed"

# --- an order-of-magnitude slow call fails ----------------------------------
# threshold for dns.lookup-literal is 300ms; 2s must fail.
if FAKE_DELAY=2 run --only dns.lookup-literal >/dev/null 2>&1; then
  fail "a 2s dns.lookup-literal did not fail the 300ms threshold"
fi
pass "an over-threshold probe fails (the dns 8s case)"

out=$(FAKE_DELAY=2 run --only dns.lookup-literal 2>&1 || true)
printf '%s' "$out" | grep -q 'OVER THRESHOLD' && pass "reports which probe is over" \
  || fail "did not report OVER THRESHOLD: $out"

# --- baseline gate: catches a regression the threshold alone would allow -----
# 0.4s is under the 800ms fs threshold, so it passes on thresholds...
FAKE_DELAY=0.4 run --only fs.readFileSync --save "$tmp/base.json" >/dev/null 2>&1 \
  && pass "baseline recorded while under threshold" || fail "baseline run failed"
grep -q 'fs.readFileSync' "$tmp/base.json" || fail "baseline file has no probe"

# ...but 3x slower must trip the baseline gate even though it is still a small number.
if FAKE_DELAY=1.2 run --only fs.readFileSync --baseline "$tmp/base.json" --tolerance 2 >/dev/null 2>&1; then
  fail "a 3x slowdown did not trip the baseline gate"
fi
pass "baseline gate catches a 3x slowdown"
out=$(FAKE_DELAY=1.2 run --only fs.readFileSync --baseline "$tmp/base.json" --tolerance 2 2>&1 || true)
printf '%s' "$out" | grep -q 'REGRESSED' && pass "reports the regression multiple" \
  || fail "no REGRESSED marker: $out"

# ...and normal variance must NOT trip it.
FAKE_DELAY=0.45 run --only fs.readFileSync --baseline "$tmp/base.json" --tolerance 2 >/dev/null 2>&1 \
  && pass "small variance does not trip the gate" || fail "false positive on small variance"

# --- a probe that cannot run at all exits 2, distinctly from a slow one ------
set +e
FAKE_EXIT=7 FAKE_DELAY=0 run --only dns.lookup-literal >/dev/null 2>&1
rc=$?
set -e
[ "$rc" = 2 ] && pass "a broken probe exits 2, not 1" || fail "expected exit 2, got $rc"

# --- an unknown --only is a hard error, not a silent no-op -------------------
if run --only no-such-probe >/dev/null 2>&1; then
  fail "unknown --only should fail loudly"
fi
pass "unknown --only rejected"

echo "test_latency_probe: ok"
