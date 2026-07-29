#!/usr/bin/env bash
# Self-test for bun_corpus_runner.py's crash-resilient journal.
#
# The property that matters: a run killed part-way must leave every already
# measured file on disk, and --resume must finish the rest WITHOUT re-running or
# duplicating them. write_outputs() only runs at the end, so before the journal
# existed a killed run threw away everything it had measured — and a full bun
# corpus pass is long enough that being killed is the normal case, not the
# exception. The node runner grew the same journal after a SIGTERM lost a
# 4433-file run.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
runner="$repo_root/tools/integration/bun_corpus_runner.py"
tmp=$(mktemp -d)
trap 'pkill -9 -f "bun_corpus_runner.py --bin $tmp" 2>/dev/null || true; rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# Fake mbun: emits bun-test-shaped output so classify() sees a real result, and
# sleeps so we can reliably kill the run mid-flight.
cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
sleep "${FAKE_DELAY:-0}"
printf ' 2 pass\n 0 fail\n 2 expect() calls\nRan 2 tests across 1 files.\n'
exit 0
EOF
chmod +x "$tmp/fake-mbun"

mkdir -p "$tmp/corpus"
for i in 1 2 3 4 5 6 7 8; do
  printf 'test("x", () => {});\n' >"$tmp/corpus/f$i.test.ts"
  printf '%s\n' "$tmp/corpus/f$i.test.ts" >>"$tmp/list.txt"
done

run() { python3 "$runner" --bin "$tmp/fake-mbun" --root "$tmp" --cwd "$tmp" \
          --list "$tmp/list.txt" --out "$tmp/out" --timeout 20 \
          --allow-missing-node-modules "$@"; }

# --- a killed run keeps what it measured ------------------------------------
FAKE_DELAY=1 run --jobs 1 --resume >"$tmp/run1.log" 2>&1 &
runner_pid=$!
for _ in $(seq 1 100); do
  # `grep -c` exits 1 on zero matches AND prints 0, so a `|| echo 0` fallback
  # yields "0\n0" and breaks the numeric test. wc -l has no such wrinkle.
  if [ -f "$tmp/out/results.partial.tsv" ] &&
     [ "$(wc -l <"$tmp/out/results.partial.tsv")" -ge 2 ]; then break; fi
  sleep 0.2
done
kill -9 "$runner_pid" 2>/dev/null || true
wait "$runner_pid" 2>/dev/null || true

journaled=$(wc -l <"$tmp/out/results.partial.tsv")
[ "$journaled" -ge 2 ] || fail "journal kept $journaled lines after the kill, expected >=2"
pass "a killed run leaves its measured files in the journal ($journaled of 8)"
[ -f "$tmp/out/summary.json" ] && fail "summary.json written despite the kill" || true
pass "summary.json is absent, so the journal is the only survivor"

# --- --resume finishes the rest, without duplicating ------------------------
out=$(FAKE_DELAY=0 run --jobs 2 --resume 2>&1) || fail "resume run failed: $out"
printf '%s' "$out" | grep -q "resuming: $journaled already measured" \
  || fail "resume did not report $journaled already measured: $out"
pass "resume reports what it is skipping"

total=$(python3 -c "import json;print(json.load(open('$tmp/out/summary.json'))['files'])")
[ "$total" = 8 ] || fail "expected 8 files in summary, got $total"
pass "resume completes the full set (8 files)"

python3 - "$tmp/out/results.tsv" <<'PY' || exit 1
import sys
rows = [l.split("\t")[0] for l in open(sys.argv[1]).read().splitlines()[1:]]
assert len(rows) == len(set(rows)), f"duplicate rows: {len(rows)} vs {len(set(rows))} unique"
assert rows == sorted(rows), "results.tsv is not in stable path order"
print("ok   - no duplicate rows, and output is in stable path order")
PY

# --- a fresh (non-resume) run must NOT inherit a stale journal --------------
before=$(wc -l <"$tmp/out/results.partial.tsv")
[ "$before" = 8 ] || fail "journal should hold 8 before the fresh run, holds $before"
FAKE_DELAY=0 run --jobs 2 >/dev/null 2>&1 || fail "fresh run failed"
after=$(wc -l <"$tmp/out/results.partial.tsv")
[ "$after" = 8 ] || fail "fresh run left $after journal lines, expected it to truncate and rewrite 8"
pass "a run without --resume truncates the journal instead of appending to it"

echo "test_bun_runner_journal: ok"
