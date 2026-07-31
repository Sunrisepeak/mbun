#!/usr/bin/env bash
# Self-test for dispatch_gate.py. The properties that matter: it must never
# exceed the campaign ceiling, must refuse rather than warn when the box cannot
# take a wave, must name WHICH limit bound the answer, and must not depend on
# live machine state (a guard that only fires on a full disk is untestable).
set -euo pipefail

tool="$(cd "$(dirname "$0")/.." && pwd)/dispatch_gate.py"

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# A roomy box, pinned so the suite does not depend on the real machine.
roomy() {
  MBUN_GATE_FAKE_DISK_GB=200 MBUN_GATE_FAKE_MEM_GB=64 \
  MBUN_GATE_FAKE_LOAD=1 MBUN_GATE_FAKE_CORES=32 python3 "$tool" "$@"
}

# --- the ceiling is never exceeded, however much room there is ---------------
o=$(MBUN_GATE_FAKE_DISK_GB=100000 MBUN_GATE_FAKE_MEM_GB=10000 \
    MBUN_GATE_FAKE_LOAD=0 MBUN_GATE_FAKE_CORES=256 python3 "$tool")
echo "$o" | grep -qE '=> 5 lane\(s\) may start now' \
  || fail "an enormous box must still cap at the campaign ceiling of 5: $(echo "$o" | tail -1)"
pass "the campaign ceiling of 5 is never exceeded"

# --- a roomy box allows a full wave -----------------------------------------
o=$(roomy)
echo "$o" | grep -qE '=> 5 lane\(s\)' || fail "a roomy box must allow 5: $(echo "$o" | tail -1)"
roomy --want 5 >/dev/null || fail "--want 5 must succeed on a roomy box"
pass "a roomy box allows a full wave of 5"

# --- a nearly-full disk is the binding limit, and says so --------------------
# 20 GB free: (20 - 15 floor) / 8 per lane = 0 lanes.
o=$(MBUN_GATE_FAKE_DISK_GB=20 MBUN_GATE_FAKE_MEM_GB=64 \
    MBUN_GATE_FAKE_LOAD=1 MBUN_GATE_FAKE_CORES=32 python3 "$tool" || true)
echo "$o" | grep -qE '=> 0 lane\(s\)' || fail "20 GB free must allow 0 lanes: $(echo "$o" | tail -1)"
echo "$o" | grep -q 'disk' || fail "the report must name the disk limit"
pass "a nearly-full disk allows 0 lanes and the disk line is reported"

# --- refusal is an EXIT CODE, not a warning ---------------------------------
set +e
MBUN_GATE_FAKE_DISK_GB=20 MBUN_GATE_FAKE_MEM_GB=64 \
MBUN_GATE_FAKE_LOAD=1 MBUN_GATE_FAKE_CORES=32 python3 "$tool" --want 5 >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "an unsafe --want must exit 1, got $rc"
pass "an unsafe --want refuses with exit 1 rather than warning"

# --- and it names the remedy, not just the refusal ---------------------------
o=$(MBUN_GATE_FAKE_DISK_GB=20 MBUN_GATE_FAKE_MEM_GB=64 MBUN_GATE_FAKE_LOAD=1 \
    MBUN_GATE_FAKE_CORES=32 python3 "$tool" --want 5 2>&1 || true)
echo "$o" | grep -q 'reclaim_disk' || fail "a 0-lane refusal must name reclaim_disk.sh"
pass "a 0-lane refusal names the remedy"

# --- memory can bind independently of disk ----------------------------------
o=$(MBUN_GATE_FAKE_DISK_GB=500 MBUN_GATE_FAKE_MEM_GB=12 \
    MBUN_GATE_FAKE_LOAD=1 MBUN_GATE_FAKE_CORES=32 python3 "$tool")
echo "$o" | grep -qE '=> 2 lane\(s\)' \
  || fail "12 GB avail / 6 GB per lane must bind at 2: $(echo "$o" | tail -1)"
pass "memory binds independently of disk"

# --- a loaded box is refused even with disk and RAM to spare -----------------
# This is the subtle one: load does not merely slow a wave, it fabricates
# results -- the same binary has measured pass-idle/fail-under-load on 60 files.
o=$(MBUN_GATE_FAKE_DISK_GB=500 MBUN_GATE_FAKE_MEM_GB=64 \
    MBUN_GATE_FAKE_LOAD=30 MBUN_GATE_FAKE_CORES=32 python3 "$tool")
echo "$o" | grep -qE '=> 0 lane\(s\)' \
  || fail "load 30/32 cores must allow 0 lanes even with disk+RAM free: $(echo "$o" | tail -1)"
pass "an already-loaded box is refused even with disk and RAM to spare"

# --- the tool runs against the REAL machine without crashing ----------------
python3 "$tool" >/dev/null 2>&1 || fail "must run against live machine state"
pass "runs against live machine state"

printf '\nall dispatch_gate self-tests passed\n'
