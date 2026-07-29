#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Fake mbun: behavior keyed on the test file name it is asked to execute.
cat >"$tmp/fake-mbun" <<'EOF'
#!/usr/bin/env bash
if [ -n "$FAKE_CWD_LOG" ]; then
  printf '%s\n' "$PWD" >>"$FAKE_CWD_LOG"
fi
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

# Node's upstream test paths are relative to the Node checkout, not the
# repository root. The runner must derive that execution cwd from the corpus
# shape rather than assuming its --root is also the upstream checkout.
cwd_corpus="$tmp/cwd-fixture/node/test/parallel"
mkdir -p "$cwd_corpus"
echo "// pass" >"$cwd_corpus/test-cwd.js"
cwd_log="$tmp/cwd.log"
: >"$cwd_log"
FAKE_CWD_LOG="$cwd_log" python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus cwd-fixture/node/test/parallel \
  --out "$tmp/out-cwd" --jobs 1 --timeout 0.1 >/dev/null
expected_cwd=$(cd "$cwd_corpus/../.." && pwd)
[ "$(cat "$cwd_log")" = "$expected_cwd" ] || {
  echo "expected corpus root cwd $expected_cwd, got $(cat "$cwd_log")" >&2
  exit 1
}

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

# --resume + --max-seconds complete a corpus across several bounded invocations.
# A zero budget must dispatch nothing and report the whole set as remaining;
# resuming afterwards must keep what landed and run only the rest.
python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --out "$tmp/out-partial" --jobs 1 --timeout 0.1 --max-seconds 0.0001 >/dev/null

python3 - "$tmp/out-partial" <<'PY'
import json, pathlib, sys
s = json.loads((pathlib.Path(sys.argv[1]) / "summary.json").read_text())
assert s["files"] == 0, s
assert s["incomplete"] is True and s["remaining"] == 4, s
PY

python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp" --corpus corpus/parallel \
  --out "$tmp/out-partial" --jobs 2 --timeout 0.1 --resume >/dev/null

python3 - "$tmp/out-partial" <<'PY'
import json, pathlib, sys
s = json.loads((pathlib.Path(sys.argv[1]) / "summary.json").read_text())
assert s["files"] == 4, s
assert "incomplete" not in s, s
assert s["categories"] == {"fail": 1, "pass": 1, "skipped": 1, "timeout": 1}, s
PY

# Resuming a complete run re-runs nothing and preserves the recorded results.
before=$(cat "$tmp/out-partial/results.tsv")
python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin /nonexistent-binary --root "$tmp" --corpus corpus/parallel \
  --out "$tmp/out-partial" --jobs 2 --timeout 0.1 --resume >/dev/null
[ "$before" = "$(cat "$tmp/out-partial/results.tsv")" ] \
  || { echo "resume of a complete run must be a no-op" >&2; exit 1; }

# A run KILLED mid-corpus must keep what it already measured. Writing results
# only at the end made the whole run all-or-nothing, which is the very failure
# --resume exists to prevent: a full corpus outlives an agent turn and the
# SIGTERM at that boundary threw away everything.
kill_corpus="$tmp/killcorpus/parallel"
mkdir -p "$kill_corpus"
for i in $(seq 1 12); do echo "// slow" >"$kill_corpus/test-slow$i.js"; done
cat >"$tmp/slow-mbun" <<'EOF'
#!/usr/bin/env bash
sleep 0.4
EOF
chmod +x "$tmp/slow-mbun"

timeout -s TERM 2 python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/slow-mbun" --root "$tmp" --corpus killcorpus/parallel \
  --out "$tmp/out-killed" --jobs 1 --timeout 5 >/dev/null 2>&1 || true

partial="$tmp/out-killed/results.partial.tsv"
[ -f "$partial" ] || { echo "killed run left no journal at all" >&2; exit 1; }
rows=$(($(wc -l <"$partial") - 1))
[ "$rows" -ge 1 ] || { echo "killed run journalled no results (rows=$rows)" >&2; exit 1; }
[ "$rows" -lt 12 ] || { echo "expected the kill to interrupt the run, got all $rows" >&2; exit 1; }

# ...and resuming from that journal finishes the rest without redoing them.
python3 "$repo_root/tools/integration/node_corpus_runner.py" \
  --bin "$tmp/slow-mbun" --root "$tmp" --corpus killcorpus/parallel \
  --out "$tmp/out-killed" --jobs 4 --timeout 5 --resume >/dev/null

python3 - "$tmp/out-killed" "$rows" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
s = json.loads((root / "summary.json").read_text())
assert s["files"] == 12, s
assert "incomplete" not in s, s
# the journal is consumed once the sorted results.tsv is written
assert not (root / "results.partial.tsv").exists()
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
