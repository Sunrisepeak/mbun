#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
out=$(mktemp)
ledger=$(mktemp)
refs=$(mktemp)
branches=$(mktemp)
fixture=$(mktemp -d)
missing_manifest=$(mktemp -d)
trap 'rm -f "$out" "$ledger" "$refs" "$branches"; rm -rf "$fixture" "$missing_manifest"' EXIT
"$repo/tools/integration/branch_inventory.sh" >"$out"
head -n 1 "$out" | grep -Fx 'branch|tip|main_ancestor|patch_equivalent|modules|worktree|remote'
grep -E '^agent/t6-2-http-lowlevel\|[0-9a-f]{8}\|no\|(yes|no)\|http_types\|' "$out"
grep -E '^agent/t6-4-sys-bindings\|[0-9a-f]{8}\|no\|(yes|no)\|sys_bindings\|' "$out"
diff -u \
  <(tail -n +2 "$out" | cut -d'|' -f1) \
  <(tail -n +2 "$out" | cut -d'|' -f1 | sort -V)

awk '
  /^```text$/ { in_inventory = 1; next }
  in_inventory && /^```$/ { exit }
  in_inventory { print }
' "$repo/docs/plan/20260713-all-modules-integration-ledger.md" >"$ledger"
diff -u "$ledger" "$out"
git for-each-ref --format='%(refname:short)' 'refs/heads/agent/t*' | sort -V >"$refs"
tail -n +2 "$out" | cut -d'|' -f1 >"$branches"
diff -u "$refs" "$branches"
[[ -z $(sort "$branches" | uniq -d) ]]
! grep -E '^[^|]*\|[^|]*\|[^|]*\|[^|]*\|[^|]*\|/' "$out"

git init -q -b main "$fixture"
git -C "$fixture" config user.name inventory-test
git -C "$fixture" config user.email inventory-test@example.invalid
printf '[workspace]\nmembers = []\n' >"$fixture/mcpp.toml"
printf 'base\n' >"$fixture/conflict.txt"
git -C "$fixture" add mcpp.toml conflict.txt
git -C "$fixture" commit -qm base
git -C "$fixture" checkout -qb agent/tmerge
printf 'shared\n' >"$fixture/conflict.txt"
git -C "$fixture" commit -am shared -q
git -C "$fixture" checkout -q main
printf 'shared\n' >"$fixture/conflict.txt"
git -C "$fixture" commit -am shared-copy -q
printf 'main\n' >"$fixture/conflict.txt"
git -C "$fixture" commit -am main-change -q
git -C "$fixture" checkout -q agent/tmerge
if git -C "$fixture" merge --no-ff main -m merge-main; then
  printf '%s\n' 'expected merge conflict' >&2
  exit 1
fi
printf 'resolution-only\n' >"$fixture/conflict.txt"
git -C "$fixture" add conflict.txt
git -C "$fixture" commit -qm merge-main
(cd "$fixture" && "$repo/tools/integration/branch_inventory.sh") \
  | grep -E '^agent/tmerge\|[0-9a-f]{8}\|no\|no\|\|\.\|no$'

git init -q -b main "$missing_manifest"
if (cd "$missing_manifest" && "$repo/tools/integration/branch_inventory.sh") >"$missing_manifest/output" 2>&1; then
  printf '%s\n' 'expected missing workspace manifest to fail' >&2
  exit 1
fi
grep -Fx 'branch_inventory.sh: root mcpp.toml is missing or unreadable' "$missing_manifest/output"
