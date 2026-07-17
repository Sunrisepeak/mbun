#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Fake mbun: behavior keyed on the test file name it is asked to execute.
cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
case "$1" in
  *test-pass*) echo ok ;;
  *test-fail*) echo boom >&2; exit 1 ;;
  *test-timeout*) sleep 10 ;;
esac
EOF
chmod +x "$tmp/fake-mbun"

corpus="$tmp/corpus/parallel"
mkdir -p "$corpus"
echo "// pass" >"$corpus/test-pass.js"
echo "// fail" >"$corpus/test-fail.js"
echo "// hang" >"$corpus/test-timeout.js"
echo "not a test" >"$corpus/other.txt"

python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --out "$tmp/out" --jobs 2 --timeout 0.1 >/dev/null

python3 - "$tmp/out" <<'PY'
import csv, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = {row["path"]: row for row in csv.DictReader((root / "results.tsv").open(), delimiter="\t")}
assert len(rows) == 3, rows
assert rows["corpus/parallel/test-pass.js"]["classification"] == "pass"
assert rows["corpus/parallel/test-fail.js"]["classification"] == "fail"
assert rows["corpus/parallel/test-timeout.js"]["classification"] == "timeout"
summary = json.loads((root / "summary.json").read_text())
assert summary["files"] == 3
assert summary["categories"] == {"fail": 1, "pass": 1, "timeout": 1}
assert all((root / row["log"]).is_file() for row in rows.values())
# The private TMPDIRs must have been cleaned up.
tmp_dir = root / "tmp"
assert not tmp_dir.exists() or not any(tmp_dir.iterdir())
PY

echo "test_node_corpus_runner: ok"
