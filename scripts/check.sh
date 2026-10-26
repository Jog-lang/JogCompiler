#!/usr/bin/env bash
# The Linux merge gate: configure, build with no warnings, run ctest, and
# syntax-check every C file with clang. Mirrors scripts/check.ps1.
# Usage, from anywhere:
#   scripts/check.sh                  build-linux/, the default compiler
#   CC=gcc scripts/check.sh           build-linux-gcc/
#   CC=clang scripts/check.sh         build-linux-clang/
#   scripts/check.sh --both           gcc, then clang
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

fail() {
    echo "check: FAILED - $1" >&2
    exit 1
}

syntax_check() {
    local sources=()
    local file base
    while IFS= read -r -d '' file; do
        base=$(basename "$file")
        if [[ "$base" == "coro_switch.c" || "$base" == "jogrt_switch.c" ]]; then
            continue
        fi
        sources+=("$file")
    done < <(find tools compiler/src tests runtime -maxdepth 1 -name '*.c' -print0)
    if [[ ${#sources[@]} -eq 0 ]]; then
        fail "clang -fsyntax-only"
    fi
    if ! clang -std=c17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
        -Itools -Icompiler/src -Iruntime -DCT_SOURCE_DIR=__FILE__ "${sources[@]}"; then
        fail "clang -fsyntax-only"
    fi
}

run_check() {
    local cc="$1"
    local dir="$2"
    local label="$3"
    local build_log=""
    local status=0

    echo "== $label ($dir) =="
    if [[ -n "$cc" ]]; then
        if ! cmake -S . -B "$dir" -G Ninja -DCMAKE_C_COMPILER="$cc" >/dev/null; then
            fail "cmake configure"
        fi
    else
        if ! cmake -S . -B "$dir" -G Ninja >/dev/null; then
            fail "cmake configure"
        fi
    fi

    set +e
    build_log=$(cmake --build "$dir" 2>&1)
    status=$?
    set -e
    printf '%s\n' "$build_log"
    if [[ $status -ne 0 ]]; then
        fail "build"
    fi
    if printf '%s\n' "$build_log" | grep -q 'warning:'; then
        fail "warnings (the build must be warning-free)"
    fi
    if ! ctest --test-dir "$dir" --output-on-failure; then
        fail "tests"
    fi
    syntax_check
    echo "check: OK"
}

if [[ "${1:-}" == "--both" ]]; then
    if [[ $# -ne 1 ]]; then
        fail "usage: scripts/check.sh [--both]"
    fi
    run_check gcc build-linux-gcc gcc
    run_check clang build-linux-clang clang
    exit 0
fi

if [[ $# -ne 0 ]]; then
    echo "usage: scripts/check.sh [--both]" >&2
    exit 1
fi

if [[ -n "${CC:-}" ]]; then
    base=$(basename "$CC")
    dir="build-linux"
    case "$base" in
        gcc|gcc-*) dir="build-linux-gcc" ;;
        clang|clang-*) dir="build-linux-clang" ;;
    esac
    run_check "$CC" "$dir" "$base"
else
    run_check "" build-linux default
fi
