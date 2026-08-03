#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

set +e
output="$({
  BUN_ZIG=/definitely/missing/bun-zig \
    BENCH3_SKIP_BUILD=1 \
    "$ROOT/benchmarks/tools/bench3.sh" string-width 1 0
} 2>&1)"
status=$?
set -e

if [ "$status" -eq 0 ]; then
  echo "expected missing bun-zig runner to fail" >&2
  exit 1
fi

if ! grep -Fq "missing required runner: bun-zig" <<<"$output"; then
  echo "expected a clear missing-runner diagnostic, got:" >&2
  echo "$output" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for runner in bun-zig bun-rust mbun; do
  ln -s "$ROOT/benchmarks/tools/bench3-fixture-runner.sh" "$tmp/$runner"
done

set +e
output="$({
  BUN_ZIG="$tmp/bun-zig" \
    BUN_RUST="$tmp/bun-rust" \
    MBUN="$tmp/mbun" \
    BENCH3_SKIP_BUILD=1 \
    "$ROOT/benchmarks/tools/bench3.sh" string-width 5 2
} 2>&1)"
status=$?
set -e

if [ "$status" -eq 0 ]; then
  echo "expected checksum mismatch to fail" >&2
  exit 1
fi
if ! grep -Fq "checksum 不一致" <<<"$output"; then
  echo "expected checksum mismatch diagnostic, got:" >&2
  echo "$output" >&2
  exit 1
fi

echo "test-bench3: ok"
