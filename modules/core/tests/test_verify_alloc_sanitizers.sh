#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 --gcc-build-dir ABSOLUTE_PATH --llvm-build-dir ABSOLUTE_PATH" >&2
    exit 2
}

gcc_build_dir=""
llvm_build_dir=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --gcc-build-dir)
            [[ $# -ge 2 ]] || usage
            gcc_build_dir="$2"
            shift 2
            ;;
        --llvm-build-dir)
            [[ $# -ge 2 ]] || usage
            llvm_build_dir="$2"
            shift 2
            ;;
        *) usage ;;
    esac
done

[[ "$gcc_build_dir" = /* && "$llvm_build_dir" = /* ]] || usage

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
verifier="$script_dir/verify_alloc_sanitizers.sh"

"$verifier" --validate-only --build-dir "$gcc_build_dir" --expect-toolchain gcc16
"$verifier" --validate-only --build-dir "$llvm_build_dir" --expect-toolchain llvm22
"$verifier" --validate-only \
    --probe "$gcc_build_dir/bin/test_core_alloc_sanitizer_probe" \
    --expect-toolchain gcc16

expect_toolchain_rejection() {
    local build_dir="$1"
    local expected_toolchain="$2"
    local log
    log="$(mktemp)"

    if "$verifier" --validate-only --build-dir "$build_dir" \
        --expect-toolchain "$expected_toolchain" >"$log" 2>&1; then
        echo "expected $expected_toolchain mismatch to be rejected: $build_dir" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi
    if ! grep -Fq "toolchain mismatch" "$log"; then
        echo "mismatch rejection did not explain the toolchain error" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi
    rm -f "$log"
}

expect_toolchain_rejection "$gcc_build_dir" llvm22
expect_toolchain_rejection "$llvm_build_dir" gcc16

echo "sanitizer verifier toolchain binding: ok"
