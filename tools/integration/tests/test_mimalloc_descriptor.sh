#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
descriptor="$repo/mcpp/pkgs/m/mbun.mimalloc.lua"
manifest="$repo/modules/core/mcpp.toml"
lockfile="$repo/modules/core/mcpp.lock"

grep -Fqx '        include_dirs = { "mimalloc-2.1.7/include" },' "$descriptor"
grep -Fqx '        sources      = { "mimalloc-2.1.7/src/static.c" },' "$descriptor"
[[ $(grep -Fc '            ["2.1.7+mbun.1"] = {' "$descriptor") -eq 3 ]]
grep -Fqx 'mimalloc = "2.1.7+mbun.1"' "$manifest"
grep -Fqx 'version = "2.1.7+mbun.1"' "$lockfile"
grep -Fqx 'source  = "index+mbun@2.1.7+mbun.1"' "$lockfile"
! grep -Fq '*/include' "$descriptor"
! grep -Fq '*/src/static.c' "$descriptor"
