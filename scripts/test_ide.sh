#!/usr/bin/env bash
# Smoke tests for `jog ide` JSON surface (Phase 1 JogLSP completeness).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOG="${JOG:-$ROOT/zig-out/bin/jog}"
FIX="$ROOT/testdata/ide"
fail=0

json_field() {
  # tiny extractor: "key": value  (string or number)
  local key="$1"
  python3 -c 'import json,sys; o=json.load(sys.stdin); k=sys.argv[1];
print(o.get(k,""))' "$key"
}

check() {
  local name="$1"
  shift
  if "$@"; then
    echo "ok  $name"
  else
    echo "FAIL $name"
    fail=1
  fi
}

# --- sibling goto: answer() in main → util.jog ---
out=$("$JOG" ide definition "$FIX/main.jog" --line 5 --column 19)
echo "$out" | grep -q '"ok":true' || { echo "FAIL definition ok: $out"; fail=1; }
echo "$out" | grep -q 'util.jog' || { echo "FAIL sibling goto file: $out"; fail=1; }
echo "$out" | grep -q '"line":2' || { echo "FAIL sibling goto line: $out"; fail=1; }
check "sibling definition" true

# --- std goto: cos → std/math/math.jog ---
out=$("$JOG" ide definition "$FIX/main.jog" --line 6 --column 19)
echo "$out" | grep -q '"ok":true' || { echo "FAIL std def ok: $out"; fail=1; }
echo "$out" | grep -q 'math.jog' || { echo "FAIL std goto file: $out"; fail=1; }
check "std definition" true

# --- hover type / signature for answer ---
out=$("$JOG" ide hover "$FIX/main.jog" --line 5 --column 19)
echo "$out" | grep -q '"ok":true' || { echo "FAIL hover ok: $out"; fail=1; }
echo "$out" | grep -q 'fn answer' || { echo "FAIL hover contents: $out"; fail=1; }
echo "$out" | grep -q 'The answer to everything' || { echo "FAIL hover docs: $out"; fail=1; }
check "hover type + docs" true

# --- hover block /** docs on Point ---
out=$("$JOG" ide hover "$FIX/util.jog" --line 11 --column 8)
echo "$out" | grep -q '"ok":true' || { echo "FAIL Point hover ok: $out"; fail=1; }
echo "$out" | grep -q '2D point' || { echo "FAIL Point hover docs: $out"; fail=1; }
check "hover block docs" true

# --- complete members after p. in util.jog use_point ---
# `    return p.x` → p at col 12, dot at 13, x at 14 (line 18 after doc comments).
out=$("$JOG" ide complete "$FIX/util.jog" --line 18 --column 14)
echo "$out" | grep -q '"ok":true' || { echo "FAIL complete ok: $out"; fail=1; }
echo "$out" | grep -q '"label":"x"' || { echo "FAIL complete x: $out"; fail=1; }
echo "$out" | grep -q '"label":"y"' || { echo "FAIL complete y: $out"; fail=1; }
check "member complete" true

# --- references for answer across loaded graph ---
out=$("$JOG" ide references "$FIX/main.jog" --line 5 --column 19)
echo "$out" | grep -q '"ok":true' || { echo "FAIL refs ok: $out"; fail=1; }
# at least util decl + main use
count=$(echo "$out" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["refs"]))')
if [[ "$count" -ge 2 ]]; then
  echo "ok  references ($count)"
else
  echo "FAIL references count=$count out=$out"
  fail=1
fi

# --- symbols include sibling + std tops ---
out=$("$JOG" ide symbols "$FIX/main.jog")
echo "$out" | grep -q '"name":"answer"' || { echo "FAIL symbols answer: $out"; fail=1; }
echo "$out" | grep -q '"name":"Point"' || { echo "FAIL symbols Point"; fail=1; }
echo "$out" | grep -q '"name":"cos"' || { echo "FAIL symbols cos"; fail=1; }
echo "$out" | grep -q 'util.jog' || { echo "FAIL symbols file util"; fail=1; }
check "graph symbols" true

# --- diag end span + severity ---
dout=$("$JOG" check --diag=json "$FIX/diag_bad.jog" 2>/dev/null || true)
echo "$dout" | grep -q '"severity":"error"' || { echo "FAIL diag severity: $dout"; fail=1; }
echo "$dout" | grep -q '"end_line"' || { echo "FAIL diag end_line: $dout"; fail=1; }
echo "$dout" | grep -q '"end_column"' || { echo "FAIL diag end_column: $dout"; fail=1; }
check "diag json fields" true

# --- flat merge: .jog-lsp-tmp must not replace sibling same-basename sources ---
FLAT="$ROOT/testdata/flat_merge"
WRAP="$(cd "$FLAT/wrap" && pwd)/init.jog"
cp "$WRAP" "$WRAP.jog-lsp-tmp"
trap 'rm -f "$WRAP.jog-lsp-tmp"' EXIT
if dout=$("$JOG" check --diag=json "$WRAP.jog-lsp-tmp" 2>/dev/null); then
  if [[ -n "${dout// }" ]]; then
    echo "FAIL flat_merge lsp-tmp diags: $dout"
    fail=1
  else
    echo "ok  flat_merge lsp-tmp check"
  fi
else
  echo "FAIL flat_merge lsp-tmp check exit: $dout"
  fail=1
fi
# Relative path under module must still load jog.mod (dirname walk → cwd).
if ! (cd "$FLAT" && "$JOG" check wrap/init.jog >/dev/null); then
  echo "FAIL relative wrap/init.jog under flat_merge"
  fail=1
else
  echo "ok  relative module-up check"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "test_ide: FAILED"
  exit 1
fi
echo "test_ide: OK"
