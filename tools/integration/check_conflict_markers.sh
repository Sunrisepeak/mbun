#!/usr/bin/env bash
# check_conflict_markers.sh -- fail if any tracked source still carries an
# unresolved merge-conflict marker.
#
# WHY THIS IS NOT REDUNDANT WITH THE COMPILER: mbun's builtins embed JavaScript
# inside C++ raw string literals (`R"JS( ... )JS"`). A conflict marker left in
# that JS is, to the C++ compiler, ordinary text — it compiles clean, links
# clean, and ships a binary whose `execSync` is syntactically broken at runtime.
# That happened during the round-9 integration: `mcpp build` reported
# "Finished release [optimized]" over a file containing four conflict markers.
# A green build is not evidence the merge was resolved.
#
# Usage:
#   tools/integration/check_conflict_markers.sh            # whole worktree
#   tools/integration/check_conflict_markers.sh <path...>  # specific paths
#
# Exits 1 and lists file:line on the first offence. Intended for CI and as the
# last step of any conflict resolution.
set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

# Markers at the start of a line, as git writes them. `=======` alone is too
# common in comment banners and documentation rules to flag on its own, so a
# file is only reported when it carries a `<<<<<<< ` or `>>>>>>> ` line.
pattern='^(<<<<<<< |>>>>>>> |\|\|\|\|\|\|\| )'

if [ $# -gt 0 ]; then
  files=$(git ls-files -- "$@")
else
  files=$(git ls-files)
fi

[ -n "$files" ] || { echo "check_conflict_markers: no tracked files to check"; exit 0; }

# -I skips binary files; the tool's own source and self-test legitimately
# contain marker text, so they are excluded by path.
hits=$(printf '%s\n' "$files" \
  | grep -v -e '^tools/integration/check_conflict_markers\.sh$' \
            -e '^tools/integration/tests/test_check_conflict_markers\.sh$' \
  | tr '\n' '\0' \
  | xargs -0 grep -InE "$pattern" -- 2>/dev/null)

if [ -n "$hits" ]; then
  echo "unresolved merge-conflict markers found:" >&2
  printf '%s\n' "$hits" >&2
  echo >&2
  echo "A clean build does NOT clear this: JS embedded in C++ raw string" >&2
  echo "literals compiles fine with markers in it and breaks at runtime." >&2
  exit 1
fi

echo "check_conflict_markers: clean"
