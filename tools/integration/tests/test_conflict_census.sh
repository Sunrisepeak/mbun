#!/usr/bin/env bash
# Self-test for conflict_census.py. The properties that matter: the census must
# separate a cross-corpus conflict from an ordinary single-corpus loss, must key
# a candidate on what mbun PRODUCED rather than on what the failing test WANTS,
# must not raise a candidate for a value the whole other corpus shares, and must
# not raise one when the two corpora agree byte-for-byte. Over-reporting is the
# failure mode that matters here: an inflated conflict list would be read as
# work that dialect dispatch has to cover, and would be sized and staffed.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
tool="$repo_root/tools/integration/conflict_census.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

r="$tmp/repo"
mkdir -p "$r/tools/integration" "$r/compat/node/test/parallel" \
         "$r/compat/bun/test/js" "$r/nrun/logs" "$r/brun/logs"
cp "$tool" "$r/tools/integration/"

# --- fixture corpora ---------------------------------------------------------
# node wants the message without a trailing stop; bun pins it WITH one. This is
# the shape of every confirmed conflict, reduced to two files.
cat > "$r/compat/node/test/parallel/test-stack.js" <<'EOF'
assert.strictEqual(err.message, 'Maximum widget depth exceeded');
EOF
cat > "$r/compat/bun/test/js/widget.test.ts" <<'EOF'
expect(err.message).toBe("Maximum widget depth exceeded.");
EOF

# A control: the failing node file also names a value it EXPECTS. That value
# must never become a candidate -- a candidate is what the binary produced.
cat > "$r/compat/node/test/parallel/test-expects.js" <<'EOF'
assert.strictEqual(e.code, 'ERR_WIDGET_ONLY_EXPECTED_NEVER_PRODUCED');
EOF
cat > "$r/compat/bun/test/js/expects.test.ts" <<'EOF'
expect(e.code).toBe("ERR_WIDGET_ONLY_EXPECTED_NEVER_PRODUCED");
EOF

# A control: a value the whole other corpus asserts is a shared convention that
# mbun already satisfies, not a conflict.
for i in 1 2 3 4 5; do
  cat > "$r/compat/bun/test/js/common$i.test.ts" <<'EOF'
expect(e.code).toBe("ERR_WIDGET_EVERYWHERE");
EOF
done

# A control: a token that appears only in a COMMENT pins nothing.
cat > "$r/compat/bun/test/js/comment.test.ts" <<'EOF'
// mbun prints Maximum sprocket depth exceeded. here, which we do not check
const x = 1;
EOF
cat > "$r/compat/node/test/parallel/test-sprocket.js" <<'EOF'
assert.strictEqual(err.message, 'Maximum sprocket depth exceeded');
EOF

# --- fixture runs ------------------------------------------------------------
printf 'path\texit_code\tclassification\tduration_ms\tlog\n' > "$r/nrun/results.tsv"
nrow() { printf 'compat/node/test/parallel/%s\t%s\t%s\t1\tlogs/%s.log\n' \
                "$1" "$3" "$2" "$1" >> "$r/nrun/results.tsv"; }
nrow test-stack.js    fail 1
nrow test-expects.js  fail 1
nrow test-sprocket.js fail 1

printf 'path\texit_code\tpassed\tfailed\texpects\tran\tclassification\tduration_ms\tlog\n' \
  > "$r/brun/results.tsv"
brow() { printf 'compat/bun/test/js/%s\t0\t1\t0\t1\t1\t%s\t1\tlogs/%s.log\n' \
                "$1" "$2" "$1" >> "$r/brun/results.tsv"; }
brow widget.test.ts  green
brow expects.test.ts green
brow comment.test.ts green
for i in 1 2 3 4 5; do brow "common$i.test.ts" green; done

# The failure logs: what the binary PRINTED. The stack file saw bun's spelling;
# the expects file saw something unrelated; the sprocket file saw the value that
# only a bun comment mentions.
echo "AssertionError: got 'Maximum widget depth exceeded.'" \
  > "$r/nrun/logs/test-stack.js.log"
echo "AssertionError: got 'ERR_WIDGET_EVERYWHERE' plus some unrelated noise" \
  > "$r/nrun/logs/test-expects.js.log"
echo "AssertionError: got 'Maximum sprocket depth exceeded.'" \
  > "$r/nrun/logs/test-sprocket.js.log"
for f in widget expects comment common1 common2 common3 common4 common5; do
  echo ok > "$r/brun/logs/$f.test.ts.log"
done

cd "$r"
census() { python3 tools/integration/conflict_census.py "$@"; }
mine() { census mine --node-run "$r/nrun" --bun-run "$r/brun" --root "$r" "$@"; }

# --- the miner keys on what was PRODUCED, not on what is expected ------------
mine --out "$tmp/m.tsv" >/dev/null 2>&1
grep -q 'Maximum widget depth exceeded\.' "$tmp/m.tsv" \
  || fail "a value a green file of the other corpus pins must be a candidate"
pass "a value pinned by the other corpus' green file is raised as a candidate"

grep -q 'ERR_WIDGET_ONLY_EXPECTED_NEVER_PRODUCED' "$tmp/m.tsv" \
  && fail "a value the failing test itself asserts is its EXPECTATION, not a conflict"
pass "a value the failing test itself asserts is not raised"

# --- a value the whole other corpus shares is a convention, not a conflict ----
mine --max-pin 3 --out "$tmp/m2.tsv" >/dev/null 2>&1
grep -q 'ERR_WIDGET_EVERYWHERE' "$tmp/m2.tsv" \
  && fail "a token pinned by more green files than --max-pin must be dropped"
pass "a token pinned corpus-wide is dropped as a shared convention"

# --- a token only a comment mentions pins nothing ----------------------------
grep -q 'Maximum sprocket depth exceeded' "$tmp/m.tsv" \
  && fail "a token appearing only in a comment must not count as pinned"
pass "a token appearing only in a comment is not treated as pinned"

# --- the pairs pass finds punctuation-only divergence, and only that ---------
census pairs --node-run "$r/nrun" --bun-run "$r/brun" --root "$r" \
        --min-words 3 --out "$tmp/p.tsv" >/dev/null 2>&1
grep -q 'Maximum widget depth exceeded' "$tmp/p.tsv" \
  || fail "pairs must find two spellings of one message across the corpora"
pass "pairs finds a punctuation-only cross-corpus divergence"

awk -F'\t' 'NR>1 && $1==$2 {found=1} END{exit !found}' "$tmp/p.tsv" \
  && fail "pairs must never report a byte-identical message as a divergence"
pass "pairs never reports a byte-identical message"

# --- the ledger separates cross-corpus from single-corpus --------------------
cat > "$r/tools/integration/struck.tsv" <<'EOF'
# header
area	target	verdict	cost_or_size	evidence
node	widget message	COSTS_GREEN	1 bun file for 1 green node file	same input, no discriminator
bun	widget preload	COSTS_GREEN	5 bun files	lost five of its own
node	widget sizing	TOO_BIG	huge	not a conflict at all
EOF
o=$(census ledger)
echo "$o" | grep -qE 'cross-corpus conflict[^:]*: 1' \
  || fail "the row whose loss lands in the OTHER corpus must count as cross-corpus"
echo "$o" | grep -qE 'single-corpus loss[^:]*: 1' \
  || fail "a row that lost only its own corpus' files is not a conflict"
pass "the ledger separates cross-corpus conflicts from single-corpus losses"

printf '\nall conflict_census self-tests passed\n'
