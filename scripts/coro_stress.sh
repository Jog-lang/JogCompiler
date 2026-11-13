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
build coro_steal_stress.jog coro_steal_stress
build coro_with_dispatcher.jog coro_with
build coro_io_dispatcher.jog coro_io

echo "== coro_with_dispatcher (gate 3) x$ROUNDS =="
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_with)" || { echo "HANG/FAIL with round $i"; exit 1; }
  echo "$out" | tail -n1 | grep -q 'with done' || { echo "FAIL with round $i"; echo "$out"; exit 1; }
  # Each cid must appear on a primary wid (0|1) and a secondary wid (2|3).
  python3 - "$out" <<'PY' || { echo "FAIL with wid ranges round $i"; exit 1; }
import re, sys, collections
out = sys.argv[1]
by = collections.defaultdict(list)
for line in out.splitlines():
    m = re.match(r"tid=\d+ wid=(\d+) cid=(\d+)", line)
    if m:
        by[int(m.group(2))].append(int(m.group(1)))
assert by, "no tid lines"
for cid, wids in by.items():
    assert any(w in (0, 1) for w in wids), (cid, wids)
    assert any(w in (2, 3) for w in wids), (cid, wids)
PY
done
echo "with-dispatcher ok"

echo "== coro_io_dispatcher (gate 4) x$ROUNDS =="
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_io)" || { echo "HANG/FAIL io round $i"; exit 1; }
  echo "$out" | tail -n1 | grep -q 'io done' || { echo "FAIL io round $i"; echo "$out"; exit 1; }
  python3 - "$out" <<'PY' || { echo "FAIL io checks round $i"; echo "$out"; exit 1; }
import re, sys
out = sys.argv[1]
lines = out.splitlines()
assert any(l == "io done" for l in lines), "missing io done"
assert any(l == "cpu" for l in lines), "missing cpu progress"
assert sum(1 for l in lines if l == "block-enter") >= 4
assert sum(1 for l in lines if l == "block-leave") >= 4
wids = []
for line in lines:
    m = re.match(r"tid=\d+ wid=(\d+) cid=(\d+)", line)
    if m:
        wids.append(int(m.group(1)))
assert wids, "no tid lines"
# Default pool (primary 2 workers) must run; unlocked print can interleave
# markers with tid lines, so attribute by wid ranges only.
assert any(w in (0, 1) for w in wids), wids
io_wids = [w for w in wids if w >= 2]
assert io_wids, wids
assert len(set(io_wids)) >= 2, ("expected elastic IO grow", io_wids)
# cpu marker while blockers are outstanding (default pool not stalled).
enter_i = next(i for i, l in enumerate(lines) if l == "block-enter")
leave_i = max(i for i, l in enumerate(lines) if l == "block-leave")
assert any(l == "cpu" for l in lines[enter_i:leave_i+1]), "cpu did not progress during blocking window"
PY
done
echo "io-dispatcher ok"

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

echo "== coro_steal_stress (Chase-Lev) x$ROUNDS =="
steal_multi=0
steal_single=0
for i in $(seq 1 "$ROUNDS"); do
  out="$(run_bounded coro_steal_stress)" || { echo "HANG/FAIL steal round $i"; exit 1; }
  echo "$out" | tail -n1 | grep -q 'steal done' || { echo "FAIL steal round $i"; echo "$out"; exit 1; }
  nt="$(echo "$out" | count_unique_tids)"
  if [[ "$nt" -gt 1 ]]; then steal_multi=$((steal_multi+1)); else steal_single=$((steal_single+1)); fi
done
echo "steal multi_tid=$steal_multi single_tid=$steal_single"

if [[ "$steal_multi" -lt 1 ]]; then
  echo "FAIL: expected at least one multi-tid steal-stress run"
  exit 1
fi

echo "OK"
