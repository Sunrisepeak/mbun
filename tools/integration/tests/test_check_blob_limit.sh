#!/usr/bin/env bash
# Self-test for check_blob_limit.py. Builds throwaway .cppm files around GCC's
# 262144-character constexpr walk limit, because the real tree has exactly one
# blob near it and no blob over it -- so the failure path has to be synthesised
# or it is never exercised until it costs somebody an afternoon.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
script="$repo_root/tools/integration/check_blob_limit.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

blob() {  # blob <file> <symbol> <length>
    { printf 'export module x;\ninline constexpr std::string_view %s = R"JS(' "$2"
      head -c "$3" /dev/zero | tr '\0' 'x'
      printf ')JS";\n'
    } >"$1"
}

mkdir -p "$tmp/mods"

# --- comfortably under the limit: silent success ----------------------------
blob "$tmp/mods/small.cppm" kSmall 1000
out=$(python3 "$script" --root "$tmp/mods" 2>&1) || fail "a small blob must not fail: $out"
printf '%s' "$out" | grep -q 'clean' && pass "small blob passes" || fail "no clean line: $out"
printf '%s' "$out" | grep -qi 'warning' && fail "warned about a small blob" || pass "small blob is not warned about"

# --- inside the margin: warn, but still exit 0 ------------------------------
blob "$tmp/mods/near.cppm" kNear 262000
out=$(python3 "$script" --root "$tmp/mods" 2>&1) || fail "a near-limit blob must not FAIL: $out"
printf '%s' "$out" | grep -q 'WARNING.*kNear.*144 below' \
  && pass "near-limit blob warns with its remaining headroom" || fail "no headroom warning: $out"

# --- over the limit: fail, and say what to do -------------------------------
blob "$tmp/mods/over.cppm" kOver 262500
if out=$(python3 "$script" --root "$tmp/mods" 2>&1); then
    fail "an over-limit blob must exit non-zero: $out"
fi
printf '%s' "$out" | grep -q 'kOver is 262500 chars, 356 OVER' \
  && pass "over-limit blob is named with its overshoot" || fail "bad overshoot message: $out"
printf '%s' "$out" | grep -q 'char_traits' \
  && pass "message names the header GCC will blame" || fail "does not mention char_traits: $out"
printf '%s' "$out" | grep -q 'Split the blob' \
  && pass "message says what to do about it" || fail "no remedy in message: $out"

# --- a non-constexpr literal is not our business ----------------------------
rm "$tmp/mods/over.cppm" "$tmp/mods/near.cppm"
{ printf 'export module x;\nstatic const char* kPlain = R"JS('
  head -c 300000 /dev/zero | tr '\0' 'x'
  printf ')JS";\n'
} >"$tmp/mods/plain.cppm"
out=$(python3 "$script" --root "$tmp/mods" 2>&1) || fail "a non-constexpr blob must not fail: $out"
pass "only constexpr string_view blobs are checked"

# --- the real tree is clean -------------------------------------------------
out=$(python3 "$script" --root "$repo_root/modules" 2>&1) \
  || fail "the repository has a blob OVER the limit: $out"
pass "repository blobs are all under the limit"

echo "test_check_blob_limit: ok"
