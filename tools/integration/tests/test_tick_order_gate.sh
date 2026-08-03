#!/usr/bin/env bash
# Self-test for tick_order_gate.py, driven by fake runtimes whose ordering output
# we control. The properties that matter: the exact inversion mbun actually
# shipped must FAIL the gate, a correct runtime must PASS, and the one invariant
# node itself is non-deterministic about must NOT be pinned.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/tick_order_gate.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# A fake runtime prints whatever is in $tmp/output and ignores the script.
make_fake() {
  cat >"$tmp/fake" <<'EOF'
#!/usr/bin/env bash
cat "$(dirname "$0")/output"
exit "${FAKE_EXIT:-0}"
EOF
  chmod +x "$tmp/fake"
}
make_fake

# The correct, node-measured output.
good() {
  cat >"$tmp/output" <<'EOF'
catch_from_tick_unhandled_count=0
deep_chain_ticks_ran=3000
microtask_inside_tick=tickA,tickB,mt
microtasks_before_tick=mt1,mt2,TICK
nested_tick_before_timer=t1,t2,timeout
tick_before_timer_and_immediate=tick,immediate,timeout
tick_fifo=1,2,3
EOF
}

run() { python3 "$tool" --bin "$tmp/fake" --timeout 10; }
# `run` is expected to exit non-zero in most cases here, and `pipefail` would make
# `run | grep` fail on that exit even when grep matched -- so capture, then grep.
run_output() { python3 "$tool" --bin "$tmp/fake" --timeout 10 2>&1 || true; }

good
run >/dev/null 2>&1 || fail "a correct runtime must pass"
pass "correct ordering passes the gate"

# The real bug: a tick queued inside a microtask ran before the rest of the chain.
good
sed -i 's/^microtasks_before_tick=.*/microtasks_before_tick=mt1,TICK,mt2/' "$tmp/output"
if run >/dev/null 2>&1; then fail "the shipped inversion must fail the gate"; fi
run_output | grep -q 'microtasks_before_tick' || fail "failure must name the broken invariant"
pass "the mt1,TICK,mt2 inversion fails the gate and is named"

# Non-determinism that node itself exhibits must not fail the gate.
good
sed -i 's/^tick_before_timer_and_immediate=.*/tick_before_timer_and_immediate=tick,timeout,immediate/' "$tmp/output"
run >/dev/null 2>&1 || fail "node's other legal immediate/timeout order must still pass"
pass "immediate-vs-timeout order is not pinned (node is non-deterministic)"

# But losing the part that IS a contract must fail.
good
sed -i 's/^tick_before_timer_and_immediate=.*/tick_before_timer_and_immediate=timeout,tick,immediate/' "$tmp/output"
if run >/dev/null 2>&1; then fail "a tick running after a timer must fail the gate"; fi
pass "a tick running after a timer fails the gate"

# Ticks must stay FIFO.
good
sed -i 's/^tick_fifo=.*/tick_fifo=1,3,2/' "$tmp/output"
if run >/dev/null 2>&1; then fail "out-of-order ticks must fail the gate"; fi
pass "out-of-order ticks fail the gate"

# A spurious unhandledRejection is fatal in mbun, so the gate must catch it.
good
sed -i 's/^catch_from_tick_unhandled_count=.*/catch_from_tick_unhandled_count=1/' "$tmp/output"
if run >/dev/null 2>&1; then fail "a spurious unhandledRejection must fail the gate"; fi
pass "spurious unhandledRejection fails the gate"

# Stack exhaustion from nested native drains shows up as a short tick count.
good
sed -i 's/^deep_chain_ticks_ran=.*/deep_chain_ticks_ran=17/' "$tmp/output"
if run >/dev/null 2>&1; then fail "a truncated deep chain must fail the gate"; fi
pass "truncated deep tick chain fails the gate"

# A runtime that reports nothing at all must fail, not silently pass.
: >"$tmp/output"
if run >/dev/null 2>&1; then fail "empty output must fail the gate"; fi
run_output | grep -q 'MISSING' || fail "empty output must report MISSING invariants"
pass "empty output fails the gate as MISSING"

# A hanging runtime is itself the regression.
cat >"$tmp/fake" <<'EOF'
#!/usr/bin/env bash
sleep 30
EOF
chmod +x "$tmp/fake"
if python3 "$tool" --bin "$tmp/fake" --timeout 2 >/dev/null 2>&1; then
  fail "a hanging runtime must fail the gate"
fi
pass "a hanging runtime fails the gate on timeout"

# A missing binary is an error, distinct from a failed invariant.
set +e
python3 "$tool" --bin "$tmp/does-not-exist" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "a missing binary must exit 2, got $rc"
pass "missing binary exits 2, distinct from an invariant failure"

# --emit-fixture must produce runnable JS for re-pinning against real node.
python3 "$tool" --emit-fixture | grep -q 'microtasks_before_tick' \
  || fail "--emit-fixture must emit the probe"
pass "--emit-fixture emits the probe for re-pinning"

printf '\nall tick_order_gate self-tests passed\n'
