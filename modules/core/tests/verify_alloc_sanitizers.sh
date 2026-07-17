#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: verify_alloc_sanitizers.sh \
  (--build-dir ABSOLUTE_PATH | --probe ABSOLUTE_PATH) \
  --expect-toolchain (gcc16|llvm22) [--validate-only]

The caller must first run `mcpp test --profile asan` with the expected
toolchain, then pass that exact build directory or probe binary. The verifier
never searches target/ or selects an artifact by modification time.
EOF
    exit 2
}

fail() {
    echo "sanitizer verifier: $*" >&2
    exit 1
}

build_dir=""
probe=""
expected_toolchain=""
validate_only=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 && -z "$build_dir" && -z "$probe" ]] || usage
            build_dir="${2%/}"
            shift 2
            ;;
        --probe)
            [[ $# -ge 2 && -z "$build_dir" && -z "$probe" ]] || usage
            probe="$2"
            shift 2
            ;;
        --expect-toolchain)
            [[ $# -ge 2 && -z "$expected_toolchain" ]] || usage
            expected_toolchain="$2"
            shift 2
            ;;
        --validate-only)
            validate_only=true
            shift
            ;;
        *) usage ;;
    esac
done

[[ -n "$build_dir" || -n "$probe" ]] || usage
[[ "$expected_toolchain" == gcc16 || "$expected_toolchain" == llvm22 ]] || usage

if [[ -n "$probe" ]]; then
    [[ "$probe" = /* ]] || fail "--probe must be an absolute path"
    [[ "${probe##*/}" == test_core_alloc_sanitizer_probe ]] ||
        fail "unexpected probe filename: $probe"
    build_dir="${probe%/bin/test_core_alloc_sanitizer_probe}"
else
    [[ "$build_dir" = /* ]] || fail "--build-dir must be an absolute path"
    probe="$build_dir/bin/test_core_alloc_sanitizer_probe"
fi

build_ninja="$build_dir/build.ninja"
[[ -f "$build_ninja" ]] || fail "missing build.ninja: $build_ninja"
[[ -x "$probe" ]] || fail "missing executable probe: $probe"
grep -Fq "test_core_alloc_sanitizer_probe" "$build_ninja" ||
    fail "build.ninja does not define the requested probe"
grep -Fq -- "-fsanitize=address" "$build_ninja" ||
    fail "build.ninja is not an AddressSanitizer build"
grep -Fq -- "-DMBUN_ALLOC_DEBUG_GUARDS=1" "$build_ninja" ||
    fail "build.ninja is missing allocator debug guards"

cxx="$(sed -n 's/^cxx[[:space:]]*=[[:space:]]*//p' "$build_ninja")"
[[ -n "$cxx" && "$cxx" != *$'\n'* ]] || fail "could not parse one cxx from build.ninja"
[[ "$cxx" = /* && -x "$cxx" ]] || fail "build.ninja cxx is not an executable absolute path: $cxx"

case "$expected_toolchain" in
    gcc16)
        compiler_version="$($cxx -dumpfullversion -dumpversion 2>/dev/null || true)"
        if [[ "${cxx##*/}" != g++ || "$compiler_version" != 16.* ]]; then
            fail "toolchain mismatch: expected gcc16, build.ninja cxx=$cxx version=$compiler_version"
        fi
        ;;
    llvm22)
        compiler_version="$($cxx --version 2>/dev/null | sed -n '1p')"
        if [[ "${cxx##*/}" != clang++ || "$compiler_version" != *"clang version 22."* ]]; then
            fail "toolchain mismatch: expected llvm22, build.ninja cxx=$cxx version=$compiler_version"
        fi
        ;;
esac

echo "validated $expected_toolchain sanitizer build: $build_dir"
if $validate_only; then
    exit 0
fi

"$probe" || fail "probe failed in normal mode"

run_probe() {
    local variable="$1"
    local expected_report="$2"
    local log
    log="$(mktemp)"

    set +e
    env "$variable=1" ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
        "$probe" >"$log" 2>&1
    local status=$?
    set -e

    if [[ $status -eq 3 ]]; then
        echo "$variable: sanitizer profile was not enabled" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi
    if [[ $status -eq 0 ]]; then
        echo "$variable: probe unexpectedly exited successfully" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi
    if ! grep -Fq "$expected_report" "$log"; then
        echo "$variable: expected report not found: $expected_report" >&2
        cat "$log" >&2
        rm -f "$log"
        return 1
    fi

    echo "$variable: verified $expected_report"
    rm -f "$log"
}

run_probe MBUN_ASAN_GLOBAL_UAF_PROBE \
    "ERROR: AddressSanitizer: heap-use-after-free"
run_probe MBUN_ASAN_ARENA_DEALLOC_UAF_PROBE \
    "ERROR: AddressSanitizer: heap-use-after-free"
run_probe MBUN_ASAN_ARENA_RESET_UAF_PROBE \
    "ERROR: AddressSanitizer: heap-use-after-free"
run_probe MBUN_LSAN_GLOBAL_LEAK_PROBE \
    "ERROR: LeakSanitizer: detected memory leaks"
run_probe MBUN_LSAN_ARENA_LEAK_PROBE \
    "ERROR: LeakSanitizer: detected memory leaks"
