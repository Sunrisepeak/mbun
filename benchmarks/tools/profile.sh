#!/usr/bin/env bash
# profile — 尽力而为的热点剖析：在可用工具里选最好的。
# 本项目环境 perf_event_paranoid=4（perf 被禁）、无 valgrind，故默认给出可用路径与指引。
#
# 用法: benchmarks/tools/profile.sh <binary> [args...]
#   优先级：perf（若权限允许）→ 提示用 prof.hpp 段计时（本环境首选）。
set -uo pipefail
BIN="${1:?usage: profile.sh <binary> [args...]}"; shift || true
ARGS=("$@")

para=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 4)
SAFE="$(cd "$(dirname "$0")/../.." && pwd)/tools/integration/safe-test.sh"
if command -v perf >/dev/null 2>&1 && [ "${para:-4}" -le 2 ]; then
  echo ">> perf record (paranoid=$para 允许) ..."
  # 被剖析的进程同样跑在资源受限 scope 里（PROFILE_TIMEOUT 秒上限）。
  "$SAFE" "${PROFILE_TIMEOUT:-600}" perf record -g --call-graph=dwarf -o /tmp/mbun-perf.data -- "$BIN" "${ARGS[@]}"
  echo ">> top hotspots:"; perf report -i /tmp/mbun-perf.data --stdio 2>/dev/null | grep -aE "^\s+[0-9]+\.[0-9]+%" | head -25
  echo "（火焰图：perf script -i /tmp/mbun-perf.data | stackcollapse-perf.pl | flamegraph.pl > fg.svg）"
  exit 0
fi

cat <<EOF
⚠️ perf 不可用（perf_event_paranoid=$para，需 <=2 或 root；本环境为 4）。valgrind 未装。

本环境的可用热点定位法（见 benchmarks/tools/PROFILING.md）：

1) 段计时（首选，无需 root）：在热路径 #include "benchmarks/tools/prof.hpp"，
   -DMBUN_PROF=1 埋 MBUN_PROF_SCOPE("名字")，跑一次 bench 看各子操作占比。
   例：定位 JSC 绑定里 字符串提取 / 内核 / JS对象物化 各占多少。

2) 前后对照：用 benchmarks/tools/bench3.sh <suite> 做优化前后的 A 级三方对照（含 checksum 闸门）。

3) 权限放开后（perf_event_paranoid<=1 或有 root）：本脚本自动切到 perf record + 火焰图。

（本次目标二进制: $BIN ${ARGS[*]}）
EOF
