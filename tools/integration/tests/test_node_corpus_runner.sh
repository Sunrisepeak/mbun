#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Fake mbun: behavior keyed on the test file name it is asked to execute.
cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
case "$1" in
  *test-skip*) echo "1..0 # Skipped: QUIC is not enabled" ;;
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
# Exits 0, but only by declining to run: must NOT be counted as a pass.
echo "// skip" >"$corpus/test-skip.js"
echo "not a test" >"$corpus/other.txt"

python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --out "$tmp/out" --jobs 2 --timeout 0.1 >/dev/null

python3 - "$tmp/out" <<'PY'
import csv, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = {row["path"]: row for row in csv.DictReader((root / "results.tsv").open(), delimiter="\t")}
assert len(rows) == 4, rows
assert rows["corpus/parallel/test-pass.js"]["classification"] == "pass"
assert rows["corpus/parallel/test-fail.js"]["classification"] == "fail"
assert rows["corpus/parallel/test-timeout.js"]["classification"] == "timeout"
# exit 0 + a TAP zero-test plan is a skip, not coverage
assert rows["corpus/parallel/test-skip.js"]["exit_code"] == "0"
assert rows["corpus/parallel/test-skip.js"]["classification"] == "skipped"
summary = json.loads((root / "summary.json").read_text())
assert summary["files"] == 4
assert summary["categories"] == {"fail": 1, "pass": 1, "skipped": 1, "timeout": 1}
assert all((root / row["log"]).is_file() for row in rows.values())
# The private TMPDIRs must have been cleaned up.
tmp_dir = root / "tmp"
assert not tmp_dir.exists() or not any(tmp_dir.iterdir())
PY

# --files scopes a run to one cluster; --filter narrows it further. Both keep
# the repo-relative path form so a cluster run stays diffable against a full one.
cat >"$tmp/worklist.txt" <<EOF
# a cluster work-list
corpus/parallel/test-pass.js
$corpus/test-fail.js
corpus/parallel/test-pass.js
EOF

python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --files "$tmp/worklist.txt" --out "$tmp/out-files" --jobs 2 --timeout 0.1 >/dev/null

python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --files "$tmp/worklist.txt" --filter test-fail --out "$tmp/out-filter" --jobs 2 --timeout 0.1 >/dev/null

python3 - "$tmp/out-files" "$tmp/out-filter" <<'PY'
import csv, pathlib, sys
def rows(d):
    return {r["path"]: r for r in csv.DictReader((pathlib.Path(d) / "results.tsv").open(), delimiter="\t")}
scoped = rows(sys.argv[1])
# absolute entry normalised, duplicate collapsed, unlisted test-timeout.js excluded
assert set(scoped) == {"corpus/parallel/test-pass.js", "corpus/parallel/test-fail.js"}, scoped
assert scoped["corpus/parallel/test-pass.js"]["classification"] == "pass"
filtered = rows(sys.argv[2])
assert set(filtered) == {"corpus/parallel/test-fail.js"}, filtered
PY

# A path that is not a file must fail loudly rather than silently shrink the run.
echo "corpus/parallel/test-missing.js" >"$tmp/bad.txt"
if python3 "$repo_root/tools/integration/node_corpus_runner.py" \
    --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
    --files "$tmp/bad.txt" --out "$tmp/out-bad" --jobs 1 --timeout 0.1 >/dev/null 2>&1; then
  echo "expected --files to reject a missing path" >&2
  exit 1
fi

echo "test_node_corpus_runner: ok"
