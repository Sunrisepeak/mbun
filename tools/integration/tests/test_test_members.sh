#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/ok" "$tmp/fail" "$tmp/empty" "$tmp/success/ok"
printf '[workspace]\nmembers = [\n    "fail",\n    "ok",\n]\n' >"$tmp/mcpp.toml"
printf '[workspace]\nmembers = []\n' >"$tmp/empty/mcpp.toml"
printf '[workspace]\nmembers = [\n    "ok",\n]\n' >"$tmp/success/mcpp.toml"
printf '#!/usr/bin/env bash\nprintf "%%s\\t%%s\\n" "$PWD" "${MCPP_TOOLCHAIN-}" >>"$TRACE"\n[[ $PWD == */ok ]]\n' >"$tmp/mcpp"
chmod +x "$tmp/mcpp"
if TRACE="$tmp/trace.tsv" MCPP_TOOLCHAIN=task-2-contract PATH="$tmp:$PATH" "$repo/tools/integration/test_members.sh" "$tmp" "$tmp/result.tsv"; then
  exit 1
fi
diff -u <(printf 'fail\tfail\nok\tpass\n') <(tail -n +2 "$tmp/result.tsv" | cut -f1,3)
diff -u <(printf '%s\t%s\n%s\t%s\n' "$tmp/fail" task-2-contract "$tmp/ok" task-2-contract) "$tmp/trace.tsv"
if "$repo/tools/integration/test_members.sh" "$tmp/missing" "$tmp/missing.tsv"; then
  exit 1
fi
if "$repo/tools/integration/test_members.sh" "$tmp/empty" "$tmp/empty.tsv"; then
  exit 1
fi
if "$repo/tools/integration/test_members.sh" "$tmp" /dev/null/result.tsv; then
  exit 1
fi
mkdir "$tmp/header.tsv"
if TRACE="$tmp/header-trace.tsv" PATH="$tmp:$PATH" "$repo/tools/integration/test_members.sh" "$tmp/success" "$tmp/header.tsv"; then
  exit 1
fi
