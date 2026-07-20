#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

tool="$repo_root/tools/integration/cluster_finder.py"
columns="path	exit_code	classification	duration_ms	log"

# A synthetic corpus run: node_corpus_runner-shaped results.tsv + per-file logs.
# The vm subsystem has THREE fixable fails that share ONE root cause (two
# identical "createScript is not a function" once volatile bits are scrubbed,
# plus a distinct assertion), one green, and one timeout. The url subsystem has
# one fixable fail. The clusterer must (a) rank vm above url by fixable count,
# (b) collapse the two createScript logs into a single 2-file cluster despite
# differing paths/quotes, and (c) never count the timeout as fixable.
mkdir -p "$tmp/logs"

{
  printf '%s\n' "$columns"
  printf 'test-vm-context.js\t1\tfail\t100\tlogs/vm1.log\n'
  printf 'test-vm-create-script.js\t1\tfail\t100\tlogs/vm2.log\n'
  printf 'test-vm-basic.js\t1\tfail\t100\tlogs/vm3.log\n'
  printf 'test-vm-ok.js\t0\tpass\t100\tlogs/vm4.log\n'
  printf 'test-vm-slow.js\t124\ttimeout\t15000\tlogs/vm5.log\n'
  printf 'test-url-parse.js\t1\tfail\t100\tlogs/url1.log\n'
} >"$tmp/results.tsv"

# Two vm logs with the SAME root cause but different volatile substrings.
printf "error: TypeError: vm.createScript is not a function. (In 'vm.createScript(%s)', 'vm.createScript' is undefined)\n" "'a.js'" >"$tmp/logs/vm1.log"
printf "error: TypeError: vm.createScript is not a function. (In 'vm.createScript(%s)', 'vm.createScript' is undefined)\n" "'/abs/other/b.js'" >"$tmp/logs/vm2.log"
printf 'error: AssertionError: Missing expected exception\n' >"$tmp/logs/vm3.log"
: >"$tmp/logs/vm4.log"
: >"$tmp/logs/vm5.log"
printf 'error: AssertionError: Missing expected exception\n' >"$tmp/logs/url1.log"

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# --- JSON mode: assert the structured clustering -----------------------------
out=$(python3 "$tool" --results "$tmp" --json)

# vm must rank first with fixable=3; timeout must NOT be in fixable.
vm_fixable=$(printf '%s' "$out" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(next(s["fixable"] for s in d["subsystems"] if s["subsystem"]=="vm"))')
[ "$vm_fixable" = "3" ] && pass "vm fixable count = 3 (timeout excluded)" || fail "vm fixable expected 3, got $vm_fixable"

vm_timeout=$(printf '%s' "$out" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(next(s["timeout"] for s in d["subsystems"] if s["subsystem"]=="vm"))')
[ "$vm_timeout" = "1" ] && pass "vm timeout counted separately = 1" || fail "vm timeout expected 1, got $vm_timeout"

# The two createScript logs must collapse into ONE cluster of size 2.
createscript_count=$(printf '%s' "$out" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(max((c["count"] for c in d["clusters"] if c["subsystem"]=="vm" and "createScript" in c["signature"]), default=0))')
[ "$createscript_count" = "2" ] && pass "two createScript logs collapsed into one 2-file cluster" || fail "createScript cluster expected size 2, got $createscript_count"

# Ranking: the largest cluster overall is the createScript one (size 2).
top_count=$(printf '%s' "$out" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["clusters"][0]["count"])')
[ "$top_count" = "2" ] && pass "largest cluster ranked first" || fail "top cluster expected size 2, got $top_count"

# --- filter mode: only url files -------------------------------------------
url_out=$(python3 "$tool" --results "$tmp" --filter test-url --json)
url_subs=$(printf '%s' "$url_out" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(",".join(sorted(s["subsystem"] for s in d["subsystems"])))')
[ "$url_subs" = "url" ] && pass "--filter restricts to matching subsystem" || fail "--filter expected only url, got '$url_subs'"

# --- human report renders without error ------------------------------------
python3 "$tool" --results "$tmp" >/dev/null && pass "human report renders" || fail "human report crashed"

echo "all cluster_finder self-tests passed"
