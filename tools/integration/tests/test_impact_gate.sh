#!/usr/bin/env bash
# Self-test for impact_gate.py. The properties that matter: a targeted gate must be
# much smaller than the corpus, must NOT be built from symbols that appear
# everywhere, must never silently imply "no regressions" when it selected nothing,
# and must refuse a diff too broad to gate.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/impact_gate.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# --- a fake repo with a fake corpus -------------------------------------------
mkdir -p "$tmp/repo/tools/integration" "$tmp/repo/compat/node/test/parallel" \
         "$tmp/repo/compat/bun/test" "$tmp/repo/run"
cp "$tool" "$tmp/repo/tools/integration/"
cd "$tmp/repo"
git init -q . && git config user.email t@t && git config user.name t

# 100 corpus files. WidgetSync appears in 3; `open` appears in 90 (so it must be
# dropped as non-discriminating); one file couples only via hasWidget.
for i in $(seq 1 100); do
  f="compat/node/test/parallel/test-f$i.js"
  echo "const x = 1; fd.open();" > "$f"
  [ "$i" -le 3 ]  && echo "new WidgetSync();" >> "$f"
  [ "$i" -eq 50 ] && echo "if (common.hasWidget) {}" >> "$f"
done
printf 'path\texit_code\tclassification\n' > run/results.tsv
for i in $(seq 1 100); do printf 'compat/node/test/parallel/test-f%s.js\t0\tpass\n' "$i" >> run/results.tsv; done
git add -A && git commit -qm base

run() { python3 tools/integration/impact_gate.py --node-run "$tmp/repo/run" "$@"; }
# Capture to a file: command substitution with `set -e` swallowed the output here.
cap() { set +e; run "$@" >"$tmp/cap.out" 2>&1; set -e; cat "$tmp/cap.out"; }

# --- a narrow change selects a narrow set -------------------------------------
cat > src.js <<'EOF'
class WidgetSync {}
EOF
git add -A && git commit -qm widget
o=$(cap --rev-range HEAD~1..HEAD)
echo "$o" | grep -qE 'node: 3 corpus files' || fail "a narrow change must select only the 3 mentioning files ($(echo "$o" | grep 'node:'))"
pass "a narrow change selects a narrow set (3 of 100)"

# --- a symbol appearing everywhere must be dropped, with its count -------------
cat > src2.js <<'EOF'
fd.open = function () {};
EOF
git add -A && git commit -qm openchange
o=$(cap --rev-range HEAD~1..HEAD)
echo "$o" | grep -q 'dropped as non-discriminating' || fail "a corpus-wide symbol must be reported as dropped"
echo "$o" | grep -qE 'open\([0-9]+\)' || fail "the drop must show the frequency that justified it"
pass "a symbol present in most of the corpus is dropped, with its count"

# --- selecting nothing must NOT read as 'no regressions' ----------------------
cat > src3.js <<'EOF'
const zzzUnusedThing = 1;
EOF
git add -A && git commit -qm nothing
o=$(cap --rev-range HEAD~1..HEAD)
echo "$o" | grep -q "Do NOT read this as 'no regressions'" \
  || fail "an empty selection must refuse to imply a clean gate"
pass "an empty selection explicitly refuses to imply 'no regressions'"

# --- the caveat is printed every time, not only when empty --------------------
o=$(cap --rev-range HEAD~3..HEAD~2)
echo "$o" | grep -q 'CAVEAT' || fail "the caveat must print on every result"
pass "the caveat prints on every result, not just empty ones"

# --- --extra-symbol reaches a file the diff cannot name -----------------------
o=$(cap --rev-range HEAD~3..HEAD~2 --extra-symbol hasWidget --out "$tmp/o.txt")
grep -q 'test-f50.js' "$tmp/o.txt" || fail "--extra-symbol must reach a file coupled by a name absent from the diff"
pass "--extra-symbol reaches a file the diff does not name"

# This is the real limitation, pinned deliberately: without the extra symbol that
# file is NOT selected. A clean impact gate is a screen, never a proof -- exactly
# how a real regression (test-webstorage-without-sqlite) sat outside a gate built
# from its own commit.
run --rev-range HEAD~3..HEAD~2 --out "$tmp/o2.txt" >/dev/null 2>&1 || true
grep -q 'test-f50.js' "$tmp/o2.txt" && fail "fixture is wrong: f50 should NOT be reachable without the extra symbol"
pass "without --extra-symbol that file is missed (the documented limitation, pinned)"

# --- a diff too broad to gate must refuse, not emit a huge set ----------------
{ for i in $(seq 1 80); do echo "class Sym$i {}"; done; } > src4.js
git add -A && git commit -qm broad
set +e
run --rev-range HEAD~1..HEAD --max-symbols 10 >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 3 ] || fail "an over-broad diff must exit 3, got $rc"
o=$(cap --rev-range HEAD~1..HEAD --max-symbols 10)
echo "$o" | grep -q 'FULL run' || fail "the refusal must point at the full run as the alternative"
pass "an over-broad diff refuses with exit 3 and names the alternative"

# --- comment-only additions expose nothing ------------------------------------
cat > src5.js <<'EOF'
// WidgetSync is mentioned only in this comment and must not be extracted
EOF
git add -A && git commit -qm commentonly
o=$(cap --rev-range HEAD~1..HEAD)
echo "$o" | grep -qE 'node: 0 corpus files' || fail "a comment-only diff must expose no symbols"
pass "comment-only additions expose nothing"

printf '\nall impact_gate self-tests passed\n'
