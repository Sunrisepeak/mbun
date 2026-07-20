#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
case "$2" in
  *green*) printf '  2 pass\n  0 fail\n  3 expect() calls\nRan 2 tests across 1 file.\n' ;;
  *red*) printf '  1 pass\n  1 fail\n  2 expect() calls\nRan 2 tests across 1 file.\n'; exit 1 ;;
  *timeout*|*blockedsvc*) sleep 10 ;;  # > RuntimeMaxSec (int(0.1)+3) and > the python fallback (0.1+8)
  *dependency*) printf "Cannot find module 'x': cannot find package 'x'\n"; exit 1 ;;
  *fixture*) printf 'Docker Compose file not found at: fixture.yml\n'; exit 1 ;;
  *native-build*) printf 'node-gyp build in fixture failed:\n'; exit 1 ;;
  # Ran tests and passed them, but also reported an out-of-test error.
  *outoftest*) printf '# Unhandled error between tests\nerror: boom\n  1 pass\n  0 fail\n  1 error\n  1 expect() calls\nRan 1 test across 1 file.\n' ;;
  # Loaded cleanly, declares no tests (comment-only / type-only corpus files).
  *notests*) printf '  0 pass\n  0 fail\nRan 0 tests across 1 file. [12.00ms]\n' ;;
  # Ran a test, then a native fault -- the crash handler prints its banner.
  *crash*) printf '  1 pass\n  1 fail\nRan 2 tests across 1 file.\n=== mbun crashed: SIGABRT (abort) ===\n--- raw backtrace ---\n=== end mbun crash report ===\n'; exit 134 ;;
  # Exits 0 too, but died before registering anything -- still a load error.
  *unhandled*) printf '# Unhandled error between tests\nerror: X is not a function\n  0 pass\n  0 fail\n  1 error\nRan 0 tests across 1 file. [9.00ms]\n' ;;
  *) printf 'error: test file evaluation error\nRan 0 tests (file did not load/run).\n'; exit 1 ;;
esac
EOF
chmod +x "$tmp/fake-mbun"
printf '%s\n' green.test.ts red.test.ts load.test.ts timeout.test.ts \
  dependency.test.ts fixture.test.ts native-build.test.ts notests.test.ts \
  unhandled.test.ts outoftest.test.ts blockedsvc.test.ts crash.test.ts >"$tmp/list.txt"

# A file that only times out because a service is missing is reported as
# blocked-external, and ONLY when it is on the manifest.
printf '%s\n' '# self-test manifest' 'blockedsvc.test.ts' >"$tmp/blocked.txt"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$repo_root" --list "$tmp/list.txt" \
  --blocked-manifest "$tmp/blocked.txt" --allow-missing-node-modules \
  --out "$tmp/out" --jobs 2 --timeout 0.1 >/dev/null

python3 - "$tmp/out" <<'PY'
import csv, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = list(csv.DictReader((root / "results.tsv").open(), delimiter="\t"))
assert [row["classification"] for row in rows] == [
    "green", "test-failure", "load-error", "timeout", "missing-dependency",
    "missing-fixture", "fixture-build-error", "no-tests", "load-error", "test-failure",
    "blocked-external", "crash",
], [row["classification"] for row in rows]
assert [int(row["passed"]) for row in rows] == [2, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1]
summary = json.loads((root / "summary.json").read_text())
assert summary["files"] == 12
assert summary["passed"] == 5 and summary["failed"] == 2
assert summary["categories"] == {
    "blocked-external": 1, "crash": 1, "fixture-build-error": 1, "green": 1,
    "load-error": 2, "no-tests": 1, "missing-dependency": 1,
    "missing-fixture": 1, "test-failure": 2, "timeout": 1,
}, summary["categories"]
assert all((root / row["log"]).is_file() for row in rows)
assert (root / "selected-tests.txt").read_text().splitlines() == [row["path"] for row in rows]
PY

mkdir -p "$tmp/corpus/a/b" "$tmp/corpus/a/c" "$tmp/corpus/node_modules/pkg"
touch "$tmp/corpus/a/b/one.test.ts" "$tmp/corpus/a/b/two.test.js" \
  "$tmp/corpus/a/b/not-a-test.ts" "$tmp/corpus/a/c/three.test.ts" \
  "$tmp/corpus/node_modules/pkg/vendor.test.ts"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --discover "$tmp/corpus" --allow-missing-node-modules \
  --sample-per-group 1 --out "$tmp/discovered" --jobs 1 >/dev/null

test "$(wc -l <"$tmp/discovered/selected-tests.txt")" -eq 2
grep -Eq '^corpus/a/b/(one\.test\.ts|two\.test\.js)$' "$tmp/discovered/selected-tests.txt"
grep -Fxq 'corpus/a/c/three.test.ts' "$tmp/discovered/selected-tests.txt"
# An installed package's own tests are not part of the corpus.
! grep -q node_modules "$tmp/discovered/selected-tests.txt"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --discover "$tmp/corpus" --allow-missing-node-modules \
  --sample-per-group 1 --max-files 1 --out "$tmp/capped" --jobs 1 >/dev/null
test "$(wc -l <"$tmp/capped/selected-tests.txt")" -eq 1

echo "test_bun_corpus_runner: ok"
