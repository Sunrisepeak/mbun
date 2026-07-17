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
  *timeout*) sleep 10 ;;  # > RuntimeMaxSec (int(0.1)+3) and > the python fallback (0.1+8)
  *dependency*) printf "Cannot find module 'x': cannot find package 'x'\n"; exit 1 ;;
  *fixture*) printf 'Docker Compose file not found at: fixture.yml\n'; exit 1 ;;
  *native-build*) printf 'node-gyp build in fixture failed:\n'; exit 1 ;;
  *) printf 'error: test file evaluation error\nRan 0 tests (file did not load/run).\n'; exit 1 ;;
esac
EOF
chmod +x "$tmp/fake-mbun"
printf '%s\n' green.test.ts red.test.ts load.test.ts timeout.test.ts \
  dependency.test.ts fixture.test.ts native-build.test.ts >"$tmp/list.txt"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$repo_root" --list "$tmp/list.txt" \
  --out "$tmp/out" --jobs 2 --timeout 0.1 >/dev/null

python3 - "$tmp/out" <<'PY'
import csv, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = list(csv.DictReader((root / "results.tsv").open(), delimiter="\t"))
assert [row["classification"] for row in rows] == [
    "green", "test-failure", "load-error", "timeout", "missing-dependency",
    "missing-fixture", "fixture-build-error",
]
assert [int(row["passed"]) for row in rows] == [2, 1, 0, 0, 0, 0, 0]
summary = json.loads((root / "summary.json").read_text())
assert summary["files"] == 7
assert summary["passed"] == 3 and summary["failed"] == 1
assert summary["categories"] == {
    "fixture-build-error": 1, "green": 1, "load-error": 1,
    "missing-dependency": 1, "missing-fixture": 1, "test-failure": 1,
    "timeout": 1,
}
assert all((root / row["log"]).is_file() for row in rows)
assert (root / "selected-tests.txt").read_text().splitlines() == [row["path"] for row in rows]
PY

mkdir -p "$tmp/corpus/a/b" "$tmp/corpus/a/c"
touch "$tmp/corpus/a/b/one.test.ts" "$tmp/corpus/a/b/two.test.js" \
  "$tmp/corpus/a/b/not-a-test.ts" "$tmp/corpus/a/c/three.test.ts"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --discover "$tmp/corpus" \
  --sample-per-group 1 --out "$tmp/discovered" --jobs 1 >/dev/null

test "$(wc -l <"$tmp/discovered/selected-tests.txt")" -eq 2
grep -Eq '^corpus/a/b/(one\.test\.ts|two\.test\.js)$' "$tmp/discovered/selected-tests.txt"
grep -Fxq 'corpus/a/c/three.test.ts' "$tmp/discovered/selected-tests.txt"

python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --discover "$tmp/corpus" \
  --sample-per-group 1 --max-files 1 --out "$tmp/capped" --jobs 1 >/dev/null
test "$(wc -l <"$tmp/capped/selected-tests.txt")" -eq 1

echo "test_bun_corpus_runner: ok"
