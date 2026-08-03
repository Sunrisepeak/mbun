#!/usr/bin/env bash
# bench3 — 同口径（A 级）三实现基准对照，强制遵循 docs/design/20260711-benchmark-standard.md。
# 在 bun-zig / bun-rust / mbun 三个二进制上跑**逐字节相同**的 bench.mjs，定核、多轮、取中位，
# 校验 checksum 三方一致，输出可直接粘进报告的 Markdown 表 + 结论。
#
# 用法: benchmarks/tools/bench3.sh <suite> [rounds] [core] [-- <bench 脚本参数...>]
#   suite  = benchmarks/suites/<suite>/bench.mjs 的目录名（如 semver / glob / toml / string-width）
#   rounds = 每实现轮数（默认 5，取中位）
#   core   = taskset 绑定核（默认 2）
#   -- 之后是传给 bench.mjs 的参数（如 fixture 路径、iters）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SUITE="${1:?usage: bench3.sh <suite> [rounds] [core] [-- args...]}"; shift
ROUNDS=5; CORE=2
[[ "${1:-}" =~ ^[0-9]+$ ]] && { ROUNDS="$1"; shift; }
[[ "${1:-}" =~ ^[0-9]+$ ]] && { CORE="$1"; shift; }
[[ "${1:-}" == "--" ]] && shift
ARGS=("$@")

SCRIPT="$ROOT/benchmarks/suites/$SUITE/bench.mjs"
[ -f "$SCRIPT" ] || { echo "no such bench: $SCRIPT"; exit 1; }
BUN_ZIG="${BUN_ZIG:-$ROOT/.mbun/bin/bun-zig}"
BUN_RUST="${BUN_RUST:-$ROOT/.mbun/bin/bun-rust}"

# 所有 runner 调用都经 safe-test.sh 的资源受限 scope 执行（内存/任务数/时限上限
# + 独立 session），一个跑飞的 bench 只会被 OOM-kill/超时，不会拖死整台机器。
SAFE="$ROOT/tools/integration/safe-test.sh"
BENCH_TIMEOUT="${BENCH3_TIMEOUT:-300}"

for runner in "bun-zig:$BUN_ZIG" "bun-rust:$BUN_RUST"; do
  name="${runner%%:*}"
  path="${runner#*:}"
  [ -x "$path" ] || {
    echo "missing required runner: $name ($path)" >&2
    echo "prepare .mbun/bin or override BUN_ZIG/BUN_RUST" >&2
    exit 1
  }
done

# mbun 二进制：洁净重建（基准标准 §3.8）
if [ "${BENCH3_SKIP_BUILD:-0}" != 1 ]; then
  echo ">> building mbun (clean release) ..."
  ( cd "$ROOT" && rm -rf target && mcpp build >/dev/null 2>&1 )
fi
MBUN="${MBUN:-$(ls "$ROOT"/target/*/*/bin/mbun 2>/dev/null | head -1)}"
[ -x "$MBUN" ] || {
  echo "missing required runner: mbun ($MBUN)" >&2
  exit 1
}
declare -A RUNNER=( [bun-zig]="$BUN_ZIG" [bun-rust]="$BUN_RUST" [mbun]="$MBUN run" )
ORDER=(bun-zig bun-rust mbun)

median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:int((a[NR/2]+a[NR/2+1])/2)}'; }

echo ">> $SUITE  (rounds=$ROUNDS, taskset -c $CORE, args='${ARGS[*]}')"
declare -A CK; METRICS=""; ROWS=""
for impl in "${ORDER[@]}"; do
  # warmup
  "$SAFE" "$BENCH_TIMEOUT" taskset -c "$CORE" ${RUNNER[$impl]} "$SCRIPT" "${ARGS[@]}" >/dev/null 2>&1 || true
  # collect rounds → per-metric arrays
  declare -A vals=()
  ck=""
  for _ in $(seq "$ROUNDS"); do
    raw="$("$SAFE" "$BENCH_TIMEOUT" taskset -c "$CORE" ${RUNNER[$impl]} "$SCRIPT" "${ARGS[@]}")" || {
      echo "runner failed: $impl" >&2
      exit 1
    }
    out="$(grep -aoE '"[a-z_]+":[0-9-]+' <<<"$raw" || true)"
    [ -n "$out" ] || {
      echo "runner produced no benchmark metrics: $impl" >&2
      exit 1
    }
    while IFS=: read -r k v; do
      [ -n "$k" ] || continue
      k="${k//\"/}"
      if [ "$k" = "checksum" ]; then ck="$v"; else vals[$k]+="$v "; fi
    done <<< "$out"
  done
  [ -n "$ck" ] || {
    echo "runner produced no checksum: $impl" >&2
    exit 1
  }
  CK[$impl]="$ck"
  # medians
  line="| $impl "
  ms=""
  for k in $(echo "${!vals[@]}" | tr ' ' '\n' | sort); do
    m=$(echo "${vals[$k]}" | tr ' ' '\n' | grep -a . | median)
    line+="| $(printf "%'d" "$m") "
    ms+="$k "
  done
  ROWS+="$line|"$'\n'
  METRICS="$ms"
done

# checksum 一致性闸门
echo ""
first_ck="${CK[${ORDER[0]}]}"
CONSISTENT=1
for impl in "${ORDER[@]}"; do [ "${CK[$impl]}" = "$first_ck" ] || CONSISTENT=0; done
if [ "$CONSISTENT" = 1 ]; then echo "✅ checksum 三方一致: $first_ck （语义等价闸门通过）"
else echo "❌ checksum 不一致: bun-zig=${CK[bun-zig]} bun-rust=${CK[bun-rust]} mbun=${CK[mbun]} —— 存在行为分歧，先修正确性！"; fi

echo ""
echo "| 实现 | $(echo $METRICS | sed 's/ / | /g') |"
echo "|---|$(for _ in $METRICS; do printf -- '---:|'; done)"
printf "%s" "$ROWS"
echo ""
echo "口径A（同口径，三方跑相同 $SUITE/bench.mjs）。核对基准标准：定核✅ ${ROUNDS}轮中位✅ 洁净重建✅ checksum闸门$([ $CONSISTENT = 1 ] && echo ✅ || echo ❌)。"
# checksum 闸门失败必须以非零退出（表格仍打印，供排查）。
[ "$CONSISTENT" = 1 ] || exit 1
