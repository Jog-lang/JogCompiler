#!/usr/bin/env bash
# JOG-230: smoke `jog bindgen` against testdata/bindgen/sample.h
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

JOG="${JOG:-$ROOT/zig-out/bin/jog}"
BINDGEN="${JOG_BINDGEN:-$ROOT/zig-out/bin/jog-bindgen}"
if [[ ! -x "$JOG" ]]; then
  echo "bindgen_smoke: missing $JOG — run zig build first" >&2
  exit 1
fi
if [[ ! -x "$BINDGEN" ]]; then
  echo "bindgen_smoke: missing $BINDGEN — run zig build first (needs libclang)" >&2
  exit 1
fi

out="$(mktemp)"
trap 'rm -f "$out"' EXIT

"$JOG" bindgen testdata/bindgen/sample.h --link sample -o "$out"
# Drop the Source: line's absolute vs relative path noise by normalizing header path.
# Compare against golden (exact).
if ! diff -u testdata/bindgen/sample.expected.jog "$out"; then
  echo "bindgen_smoke: output drifted from testdata/bindgen/sample.expected.jog" >&2
  exit 1
fi

export JOG_ROOT="${JOG_ROOT:-$ROOT}"
"$JOG" check "$out"
echo "bindgen_smoke: OK"
