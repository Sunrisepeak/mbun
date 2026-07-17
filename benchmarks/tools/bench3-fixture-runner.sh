#!/usr/bin/env bash
set -euo pipefail

case "${0##*/}" in
  bun-zig) checksum=101 ;;
  bun-rust) checksum=101 ;;
  mbun) checksum=202 ;;
  *) checksum=0 ;;
esac

printf '{"fixture_ops_per_s":100,"checksum":%s}\n' "$checksum"
