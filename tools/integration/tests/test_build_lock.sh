#!/usr/bin/env bash
# Self-test for build_lock.sh: the point is mutual exclusion ACROSS worktrees,
# so the test uses a real repo with a real linked worktree, not two shells in
# one directory.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
script="$repo_root/tools/integration/build_lock.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'ok   - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; exit 1; }

main="$tmp/main"
mkdir -p "$main"
git -C "$main" init -q
git -C "$main" config user.email t@example.com
git -C "$main" config user.name t
echo hi >"$main/f"
git -C "$main" add f
git -C "$main" commit -qm base
git -C "$main" worktree add -q --detach "$tmp/wt" HEAD

# A "build" that records overlap: it appends a start marker, sleeps, and appends
# an end marker. With a working lock the markers never interleave.
cat >"$tmp/fakebuild" <<'EOF'
#!/usr/bin/env bash
echo "start $1" >>"$TRACE"
sleep 1
echo "end $1" >>"$TRACE"
EOF
chmod +x "$tmp/fakebuild"

export TRACE="$tmp/trace"
: >"$TRACE"

# Two builds launched simultaneously from DIFFERENT worktrees of the same repo.
( cd "$main" && bash "$script" "$tmp/fakebuild" A ) &
p1=$!
( cd "$tmp/wt" && bash "$script" "$tmp/fakebuild" B ) &
p2=$!
wait $p1 $p2

lines=$(tr '\n' ' ' <"$TRACE")
case "$lines" in
  "start A end A start B end B "|"start B end B start A end A ")
    pass "builds in different worktrees were serialised (trace: $lines)" ;;
  *)
    fail "builds overlapped across worktrees, trace: $lines" ;;
esac

# With two slots the same pair is allowed to overlap.
: >"$TRACE"
( cd "$main" && MBUN_BUILD_SLOTS=2 bash "$script" "$tmp/fakebuild" A ) &
p1=$!
( cd "$tmp/wt" && MBUN_BUILD_SLOTS=2 bash "$script" "$tmp/fakebuild" B ) &
p2=$!
wait $p1 $p2
lines=$(tr '\n' ' ' <"$TRACE")
case "$lines" in
  "start A end A start B end B "|"start B end B start A end A ")
    fail "MBUN_BUILD_SLOTS=2 still serialised: $lines" ;;
  *)
    pass "MBUN_BUILD_SLOTS=2 permits concurrency (trace: $lines)" ;;
esac

# The wrapped command's exit code must pass through, not be swallowed.
( cd "$main" && bash "$script" bash -c 'exit 17' ) && rc=0 || rc=$?
[ "$rc" = 17 ] && pass "wrapped exit code propagates" || fail "expected 17, got $rc"

# Giving up returns EX_TEMPFAIL rather than running the command anyway.
( cd "$main" && bash "$script" sleep 4 ) &
holder=$!
sleep 0.4
marker="$tmp/ran"
rm -f "$marker"
( cd "$tmp/wt" && MBUN_BUILD_WAIT=1 bash "$script" touch "$marker" ) && rc=0 || rc=$?
[ "$rc" = 75 ] && pass "no free slot exits 75 (EX_TEMPFAIL)" || fail "expected 75, got $rc"
# Giving up must NOT run the command anyway.
[ ! -e "$marker" ] && pass "command not executed when the slot never came free" \
  || fail "command ran despite giving up"
wait $holder

# --- a build that leaves sources newer than the binary must warn loudly -------
# `mcpp build` has reported "Finished release in 0.01s" over edited .cppm files.
# A measurement taken after that silently scores the PREVIOUS binary, which looks
# like a result — so the wrapper has to say so.
mkdir -p "$main/modules" "$main/src" "$main/target/t/h/bin"
: >"$main/target/t/h/bin/mbun"; chmod +x "$main/target/t/h/bin/mbun"
sleep 0.05
echo "edited" >"$main/modules/thing.cppm"          # newer than the binary
warn=$( (cd "$main" && bash "$script" true) 2>&1 )
printf '%s' "$warn" | grep -q 'sources are NEWER' \
  && pass "warns when sources are newer than the built binary" \
  || fail "no staleness warning, got: $warn"
printf '%s' "$warn" | grep -q 'build.ninja -delete' \
  && pass "warning names the fix" || fail "warning does not name the fix"

# Touching the binary afterwards must silence it (no false positives).
touch "$main/target/t/h/bin/mbun"
warn2=$( (cd "$main" && bash "$script" true) 2>&1 )
printf '%s' "$warn2" | grep -q 'sources are NEWER' \
  && fail "false positive: warned with an up-to-date binary" \
  || pass "silent when the binary is up to date"

# A FAILED build must not also emit the staleness warning (one error, not two).
warn3=$( (cd "$main" && bash "$script" bash -c 'exit 3') 2>&1 || true )
printf '%s' "$warn3" | grep -q 'sources are NEWER' \
  && fail "warned on a failed build" || pass "no staleness warning on a failed build"

# --- staged libstdc++.a mismatch: repair and retry once ---------------------
# A worktree staged with a different gcc's libstdc++.a links against it (that
# directory is first on -L) and dies with `undefined reference to
# std::__cow_string::__cow_string`. The staging happens during the FIRST build,
# so no pre-check can see it -- the wrapper must recognise the signature, repair,
# and retry. Everything here is fake: no compiler runs.
gccdir="$tmp/fake/xim-x-gcc/16.1.0"
mkdir -p "$gccdir/bin" "$gccdir/lib64"
: >"$gccdir/bin/g++"
echo pinned >"$gccdir/lib64/libstdc++.a"
mkdir -p "$main/target/x86_64-linux-gnu/deadbeef"
echo "  command = $gccdir/bin/g++ -c foo.cpp" >"$main/target/x86_64-linux-gnu/deadbeef/build.ninja"
staged="$main/.mcpp/.xlings/data/xpkgs/mbun-x-mbun.jsc-prebuilt/20260706/bun-webkit/lib"
mkdir -p "$staged"

# Behaves like the real thing: the FIRST build stages the wrong archive and then
# fails linking against it; a later build reuses what is already staged. So the
# mismatch cannot exist before the build that trips over it.
cat >"$tmp/fakelink" <<EOF
#!/usr/bin/env bash
echo "attempt" >>"\$ATTEMPTS"
[ -f '$staged/libstdc++.a' ] || echo "wrong gcc" >'$staged/libstdc++.a'
if [ "\$(cat '$staged/libstdc++.a')" = pinned ]; then exit 0; fi
echo "ld: undefined reference to \\\`std::__cow_string::__cow_string(char const*)'" >&2
exit 1
EOF
chmod +x "$tmp/fakelink"
export ATTEMPTS="$tmp/attempts"; : >"$ATTEMPTS"
out=$( (cd "$main" && bash "$script" "$tmp/fakelink") 2>&1 ); rc=$?
[ "$rc" = 0 ] && pass "mismatched libstdc++.a repaired and the build retried" \
  || fail "build not recovered (rc=$rc): $out"
[ "$(wc -l <"$ATTEMPTS")" = 2 ] \
  && pass "retried exactly once" || fail "expected 2 attempts, got $(wc -l <"$ATTEMPTS")"
printf '%s' "$out" | grep -q 'repaired' \
  && pass "repair is reported, not silent" || fail "repair was silent: $out"

# An already-staged mismatch is repaired BEFORE the build, so it never fails at all.
echo "wrong gcc" >"$staged/libstdc++.a"
: >"$ATTEMPTS"
(cd "$main" && bash "$script" "$tmp/fakelink") >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && [ "$(wc -l <"$ATTEMPTS")" = 1 ] \
  && pass "a pre-existing mismatch is repaired without a failed build" \
  || fail "pre-build repair did not happen (rc=$rc, attempts=$(wc -l <"$ATTEMPTS"))"

# An unrelated failure must NOT be retried.
cat >"$tmp/failother" <<'EOF'
#!/usr/bin/env bash
echo "attempt" >>"$ATTEMPTS"
echo "error: something else entirely" >&2
exit 4
EOF
chmod +x "$tmp/failother"
: >"$ATTEMPTS"
rc=0
(cd "$main" && bash "$script" "$tmp/failother") >/dev/null 2>&1 || rc=$?
[ "$rc" = 4 ] && [ "$(wc -l <"$ATTEMPTS")" = 1 ] \
  && pass "an unrelated failure is not retried" \
  || fail "unrelated failure retried or exit code lost (rc=$rc, attempts=$(wc -l <"$ATTEMPTS"))"

echo "test_build_lock: ok"
