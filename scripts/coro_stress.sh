#!/usr/bin/env bash
# Timeout-friendly stress harness for gate-2 scheduler stability.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer the tree's freshly built jog unless JOG_STRESS is set (ignore stale $JOG).
JOG="${JOG_STRESS:-$ROOT/zig-out/bin/jog}"
OUT="$ROOT/out"
# Contention under steal storms can make a run take several seconds; 30s catches
# hard hangs without false-failing slow-but-correct runs.
TIMEOUT_SECS="${TIMEOUT_SECS:-30}"
ROUNDS="${ROUNDS:-50}"
mkdir -p "$OUT"

build() {
  local src="$1" bin="$2"
  # Paths relative to repo root so jog.mod discovery works.
  (cd "$ROOT" && "$JOG" build "examples/$src" -o "out/$bin")
}

run_bounded() {
  local bin="$1"
  timeout "${TIMEOUT_SECS}s" "$OUT/$bin"
}

count_unique_tids() {
  grep -oE 'tid=[0-9]+' | sort -u | wc -l
}

echo "== build =="
build coro_yield.jog coro_yield
build coro_pool.jog coro_pool
build coro_pool_stress.jog coro_pool_stress

echo "== coro_yield x$ROUNDS =="
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_yield)"
  echo "$out" | tr '\n' ' '
  echo
  echo "$out" | grep -q '^done$' || echo "$out" | grep -qx 'done' || {
    # accept trailing done line
    echo "$out" | tail -n1 | grep -q 'done' || { echo "FAIL yield round $i"; exit 1; }
  }
done

echo "== coro_pool x$ROUNDS =="
pool_multi=0
pool_single=0
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_pool)" || { echo "HANG/FAIL pool round $i"; exit 1; }
  echo "$out" | tail -n1 | grep -q 'pool done' || { echo "FAIL pool round $i"; echo "$out"; exit 1; }
  nt="$(echo "$out" | count_unique_tids)"
  if [[ "$nt" -gt 1 ]]; then pool_multi=$((pool_multi+1)); else pool_single=$((pool_single+1)); fi
done
echo "pool multi_tid=$pool_multi single_tid=$pool_single"

echo "== coro_pool_stress x$ROUNDS =="
stress_multi=0
stress_single=0
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_pool_stress)" || { echo "HANG/FAIL stress round $i"; exit 1; }
  echo "$out" | tail -n1 | grep -q 'stress done' || { echo "FAIL stress round $i"; echo "$out"; exit 1; }
  nt="$(echo "$out" | count_unique_tids)"
  if [[ "$nt" -gt 1 ]]; then stress_multi=$((stress_multi+1)); else stress_single=$((stress_single+1)); fi
done
echo "stress multi_tid=$stress_multi single_tid=$stress_single"

if [[ "$stress_multi" -lt 1 ]]; then
  echo "FAIL: expected at least one multi-tid stress run under N>>workers load"
  exit 1
fi

echo "OK"
