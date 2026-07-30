#!/usr/bin/env bash
# Self-test for check_struck.py. The properties that matter: a vein that was
# already retired must be FOUND (that is the whole point -- bunfig preload was
# implemented twice), a fresh target must NOT be flagged, and the exit codes must
# let a lane gate on the answer.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/check_struck.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

# rc helper: capture exit code without tripping set -e.
rc() { set +e; "$@" >"$tmp/out" 2>"$tmp/err"; local r=$?; set -e; return $r; }

printf 'area\ttarget\tverdict\tcost_or_size\tevidence\n' >"$tmp/reg.tsv"
{
  printf '# a comment line that must be ignored\n'
  printf 'bun\tbunfig preload\tCOSTS_GREEN\t5 bun files, twice\timplemented and reverted TWICE\n'
  printf 'node\ttls as a cluster target\tTOO_BIG\t62 fixable\thandshake engine is deferred\n'
  printf 'bun\tnapi addons\tBLOCKED\t51 files\tonly 6 dynamic symbols exported\n'
} >>"$tmp/reg.tsv"

run() { python3 "$tool" --registry "$tmp/reg.tsv" "$@"; }

# A retired vein must be found, and must exit 3 so a lane can gate on it.
if rc run bunfig preload; then fail "a retired vein must not exit 0"; fi
rc run bunfig preload || true
[ "$(python3 "$tool" --registry "$tmp/reg.tsv" bunfig preload >/dev/null 2>&1; echo $?)" = "3" ] \
  || fail "a retired vein must exit 3"
grep -q 'COSTS_GREEN' "$tmp/out" || fail "the verdict must be shown"
grep -q 'reverted TWICE' "$tmp/out" || fail "the evidence must be shown"
pass "a retired vein is found, exits 3, and shows verdict + evidence"

# COSTS_GREEN must be visually louder than informational verdicts.
grep -q '^!! \[COSTS_GREEN\]' "$tmp/out" || fail "COSTS_GREEN must be marked loud"
rc run napi || true
grep -q '^   \[BLOCKED\]' "$tmp/out" || fail "BLOCKED must not be marked loud"
pass "COSTS_GREEN is flagged loudly, BLOCKED is not"

# A genuinely new target must come back clean, and exit 0.
rc run zlib brotli dictionary || fail "a fresh target must exit 0"
grep -q 'go ahead' "$tmp/out" || fail "a fresh target must say go ahead"
pass "an unrecorded target exits 0"

# Terms are ANDed, so a second word narrows rather than floods.
rc run tls || true
[ "$(grep -c '^\(!!\|  \) \[' "$tmp/out")" = "1" ] || fail "one term should match one row here"
rc run tls napi || true
grep -q 'nothing on record' "$tmp/out" || fail "ANDed terms across two rows must not match"
pass "terms are ANDed, so queries narrow"

# --area restricts.
rc run --area bun || true
[ "$(grep -c '^\(!!\|  \) \[' "$tmp/out")" = "2" ] || fail "--area bun should match the 2 bun rows"
pass "--area restricts to one corpus"

# --list dumps everything and ignores the header and comment lines.
rc run --list || fail "--list must exit 0"
grep -q '3 entries' "$tmp/out" || fail "--list must count only real rows (not header/comment)"
pass "--list ignores the header and comment lines"

# A missing registry is an error, distinct from "nothing on record".
set +e
python3 "$tool" --registry "$tmp/nope.tsv" anything >/dev/null 2>&1
r=$?
set -e
[ "$r" -eq 2 ] || fail "a missing registry must exit 2, got $r"
pass "missing registry exits 2, distinct from a clean result"

# No terms and no --area is a usage error, not a silent all-clear.
set +e
python3 "$tool" --registry "$tmp/reg.tsv" >/dev/null 2>&1
r=$?
set -e
[ "$r" -ne 0 ] || fail "no query must not silently report all-clear"
pass "an empty query is a usage error, not an all-clear"

# The real registry must parse and be non-trivial.
rc python3 "$tool" --list || fail "the checked-in registry must parse"
grep -qE '[0-9]+ entries' "$tmp/out" || fail "the checked-in registry must report a count"
entries=$(grep -oE '[0-9]+ entries' "$tmp/out" | grep -oE '[0-9]+')
[ "$entries" -ge 20 ] || fail "the checked-in registry looks too small ($entries entries)"
pass "the checked-in registry parses ($entries entries)"

printf '\nall check_struck self-tests passed\n'
