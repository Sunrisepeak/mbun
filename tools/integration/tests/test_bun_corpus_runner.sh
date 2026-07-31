#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

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
# Compared as a mapping, not a sequence: the runner now emits results in stable
# PATH order (so two runs diff cleanly) rather than in --list order, and pinning
# the sequence here would only re-encode whichever order happens to be current.
def stem(row):
    return row["path"].rsplit("/", 1)[-1]


assert {stem(row): row["classification"] for row in rows} == {
    "green.test.ts": "green",
    "red.test.ts": "test-failure",
    "load.test.ts": "load-error",
    "timeout.test.ts": "timeout",
    "dependency.test.ts": "missing-dependency",
    "fixture.test.ts": "missing-fixture",
    "native-build.test.ts": "fixture-build-error",
    "notests.test.ts": "no-tests",
    "unhandled.test.ts": "load-error",
    "outoftest.test.ts": "test-failure",
    "blockedsvc.test.ts": "blocked-external",
    "crash.test.ts": "crash",
}, {stem(row): row["classification"] for row in rows}
assert {stem(row): int(row["passed"]) for row in rows if int(row["passed"])} == {
    "green.test.ts": 2, "red.test.ts": 1, "outoftest.test.ts": 1, "crash.test.ts": 1,
}
paths = [row["path"] for row in rows]
assert paths == sorted(paths), f"results.tsv is not in stable path order: {paths}"
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
assert summary["resource_profile"] == {"memory_max": "4G", "tasks_max": 512}
PY

# --- explicit resource profile is opt-in and single-lane only ----------------
printf '%s\n' green.test.ts >"$tmp/profile-list.txt"
python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$repo_root" --list "$tmp/profile-list.txt" \
  --allow-missing-node-modules --out "$tmp/profile" --jobs 1 --timeout 1 \
  --memory-max 34G --tasks-max 1024 >/dev/null
python3 - "$tmp/profile/summary.json" <<'PY'
import json, sys
summary = json.load(open(sys.argv[1]))
assert summary["resource_profile"] == {"memory_max": "34G", "tasks_max": 1024}
PY
pass "an explicit resource profile is recorded in the summary"

set +e
python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$repo_root" --list "$tmp/profile-list.txt" \
  --allow-missing-node-modules --out "$tmp/rejected-profile" --jobs 2 --timeout 1 \
  --memory-max 34G --tasks-max 1024 >"$tmp/rejected-profile.log" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "an overridden resource profile must reject jobs > 1"
grep -q -- '--jobs 1' "$tmp/rejected-profile.log" \
  || fail "the profile rejection must explain the single-lane requirement"
pass "an overridden resource profile rejects multi-lane fan-out"

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

# A worktree's vendored corpus may be a symlink to one shared checkout. The
# lexical corpus path still belongs under --root, even though resolving it
# points outside that root; discovery must keep the stable root-relative name.
mkdir -p "$tmp/symlink-root" "$tmp/symlink-target/a/b"
touch "$tmp/symlink-target/a/b/symlink.test.ts"
ln -s "$tmp/symlink-target" "$tmp/symlink-root/corpus"
python3 "$repo_root/tools/integration/bun_corpus_runner.py" \
  --bin "$tmp/fake-mbun" --root "$tmp/symlink-root" \
  --discover "$tmp/symlink-root/corpus" --allow-missing-node-modules \
  --sample-per-group 1 --out "$tmp/symlink-discovered" --jobs 1 >/dev/null
grep -Fxq 'corpus/a/b/symlink.test.ts' "$tmp/symlink-discovered/selected-tests.txt"

# --- being MORE correct than bun is not a failure ----------------------------
# bun marks cases bun itself gets wrong with `test.failing`. When mbun is more
# correct, that marker passes and the runner prints
#   (fail) <name> - expected to fail but passed
# Scoring that as `test-failure` made advancing node compatibility look like a
# bun REGRESSION, and manufactured a corpus trade-off that does not exist.
python3 - "$repo_root" <<'AHEADPY'
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(sys.argv[1]) / "tools/integration"))
from bun_corpus_runner import classify


def c(**kw):
    base = dict(exit_code=0, passed=0, failed=0, ran=0, skipped=0,
                timed_out=False, oom_killed=False, output="", blocked=False)
    base.update(kw)
    return classify(**base)


stale = "(fail) x - expected to fail but passed\n 2 pass\n 1 fail\n"
got = c(passed=2, failed=1, ran=3, output=stale)
assert got == "ahead-of-reference", got

mixed = ("(fail) x - expected to fail but passed\n"
         "(fail) y - assertion failed\n 1 pass\n 2 fail\n")
got = c(passed=1, failed=2, ran=3, output=mixed)
assert got == "test-failure", got

got = c(passed=3, failed=0, ran=3, output=" 3 pass\n 0 fail\n")
assert got == "green", got

got = c(passed=1, failed=1, ran=2, output=" 1 pass\n 1 fail\n")
assert got == "test-failure", got

# bun's runner exits 1 PRECISELY BECAUSE a failing-marked test passed, so a
# non-zero exit must not veto the ahead-of-reference verdict. Testing exit_code
# first made the bucket unreachable for the case it was added for: on the full
# corpus, assert/deep-equal.test.ts had 22 of 22 failures be "expected to fail
# but passed" and was still scored test-failure.
got = c(exit_code=1, passed=2, failed=1, ran=3, output=stale)
assert got == "ahead-of-reference", got

# ...but the exit code is forgiven ONLY when nothing else went wrong: a real
# failure alongside, or an out-of-test error count, still means test-failure.
got = c(exit_code=1, passed=1, failed=2, ran=3, output=mixed)
assert got == "test-failure", got
got = c(exit_code=1, passed=2, failed=1, ran=3, output=stale + " 1 error\n")
assert got == "test-failure", got

print("ok   - a stale test.failing marker is ahead-of-reference, not a failure")
print("ok   - a non-zero exit does not veto it (bun exits 1 because of it)")
print("ok   - a real failure or an out-of-test error still wins")
print("ok   - a real failure alongside a stale marker is still test-failure")
print("ok   - green, plain failure and non-zero exit are unaffected")
AHEADPY

echo "test_bun_corpus_runner: ok"
