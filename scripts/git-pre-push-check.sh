#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/market-data-system"

if [[ "${SKIP_PRE_PUSH_CHECK:-0}" == "1" ]]; then
  echo "[pre-push] SKIP_PRE_PUSH_CHECK=1, skip checks."
  exit 0
fi

echo "[pre-push] building..."
cmake --build "$ROOT/build" -j"$(nproc)"

if [[ ! -x "$BIN" ]]; then
  echo "[pre-push] FAIL: binary not found at $BIN" >&2
  exit 1
fi

echo "[pre-push] running offline gate..."
"$BIN" --bench-pipeline 50000 --bench-write >/tmp/mds-pre-push-bench.log 2>&1 || {
  echo "[pre-push] FAIL: offline benchmark gate failed" >&2
  tail -n 40 /tmp/mds-pre-push-bench.log >&2 || true
  exit 1
}

echo "[pre-push] PASS"
