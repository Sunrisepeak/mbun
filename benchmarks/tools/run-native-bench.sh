#!/usr/bin/env bash
# 运行 bun 原生 bench（compat/bun/bench/<suite>）——绕过 compat/bun/bench/bun.lock 里被 pin 到私有
# Artifactory（此环境 401）的 tarball，改从公共 npm 安装 mitata 等依赖到 scratch。
# bun/ 是只读语料，本脚本不改它：把 bench 脚本 + runner.mjs 复制到 .mcompat/bun/bench-scratch/
# 下、以公共 npm 装好的 node_modules 运行。
#
# 用法: benchmarks/tools/run-native-bench.sh <suite> [bun二进制...]
#   e.g. benchmarks/tools/run-native-bench.sh glob .mbun/bin/bun-zig .mbun/bin/bun-rust
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SUITE="${1:?usage: run-native-bench.sh <suite> [bun-binaries...]}"; shift || true
BINS=("$@"); [ ${#BINS[@]} -eq 0 ] && BINS=("$ROOT/.mbun/bin/bun-zig" "$ROOT/.mbun/bin/bun-rust")

# 一切 bench 执行经资源受限 scope（tools/integration/safe-test.sh）：防止跑飞
# 的 bench/依赖安装拖死机器。
SAFE="$ROOT/tools/integration/safe-test.sh"
BENCH_TIMEOUT="${NATIVE_BENCH_TIMEOUT:-300}"

SCRATCH="$ROOT/.mcompat/bun/bench-scratch"
mkdir -p "$SCRATCH"
cd "$SCRATCH"
if [ ! -d node_modules/mitata ]; then
  printf '{"name":"native-bench","dependencies":{"mitata":"1.0.20"}}\n' > package.json
  BUN_CONFIG_REGISTRY=https://registry.npmjs.org "$SAFE" 600 "${BINS[0]}" install
fi
cp "$ROOT/compat/bun/bench/runner.mjs" .
rm -rf "$SUITE" && cp -r "$ROOT/compat/bun/bench/$SUITE" "$SUITE"

for BIN in "${BINS[@]}"; do
  echo "=== $("$BIN" --version 2>/dev/null) :: compat/bun/bench/$SUITE ==="
  for f in "$SUITE"/*.mjs; do
    [ -f "$f" ] && { echo "-- $f --"; "$SAFE" "$BENCH_TIMEOUT" "$BIN" "$f" 2>&1 || echo "(failed: needs extra deps/runtime)"; }
  done
done
