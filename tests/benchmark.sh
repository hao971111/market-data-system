#!/usr/bin/env bash
# 自动性能测试：启动 live 模式跑 N 秒，解析日志并生成指标摘要。
#
# 用法：
#   bash tests/benchmark.sh                 # 默认 60 秒
#   bash tests/benchmark.sh --duration 120  # 跑 120 秒
#   bash tests/benchmark.sh --skip-e2e      # 不跑 replay 对账
#
# 输出：
#   - 控制台摘要
#   - build/benchmark_live.log
#   - build/benchmark_summary.md
#   - build/benchmark_row.md（可复制到 BENCHMARKS.md 的一行）

set -euo pipefail

DURATION=60
RUN_E2E=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            if [[ ! "${2:-}" =~ ^[0-9]+$ ]]; then
                echo "[BENCH] FAIL: --duration requires a non-negative integer, got '${2:-}'" >&2
                exit 1
            fi
            if (( $2 < 3 )); then
                echo "[BENCH] FAIL: --duration must be >= 3 seconds (got $2)" >&2
                exit 1
            fi
            DURATION="$2"
            shift 2
            ;;
        --skip-e2e)
            RUN_E2E=0
            shift
            ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *)
            echo "[BENCH] Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/market-data-system"
DATA_DIR="$ROOT/data"
LIVE_LOG="$ROOT/build/benchmark_live.log"
SUMMARY_MD="$ROOT/build/benchmark_summary.md"
ROW_MD="$ROOT/build/benchmark_row.md"

if [[ ! -x "$BIN" ]]; then
    echo "[BENCH] FAIL: binary not found at $BIN. Run 'cmake --build build' first." >&2
    exit 1
fi

echo "[BENCH] Cleaning previous data files..."
rm -f "$DATA_DIR/trades.bin" "$DATA_DIR/orderbooks.bin"

echo "[BENCH] Starting live mode for ${DURATION}s (log: $LIVE_LOG)..."
cd "$ROOT"
"$BIN" >"$LIVE_LOG" 2>&1 &
LIVE_PID=$!

sleep 2
if ! kill -0 "$LIVE_PID" 2>/dev/null; then
    echo "[BENCH] FAIL: live process exited prematurely. Tail of log:" >&2
    tail -n 30 "$LIVE_LOG" >&2 || true
    wait "$LIVE_PID" 2>/dev/null || true
    exit 1
fi

sleep "$((DURATION - 2))"
if ! kill -0 "$LIVE_PID" 2>/dev/null; then
    echo "[BENCH] FAIL: live process exited during benchmark. Tail of log:" >&2
    tail -n 30 "$LIVE_LOG" >&2 || true
    wait "$LIVE_PID" 2>/dev/null || true
    exit 1
fi

echo "[BENCH] Sending SIGINT to PID $LIVE_PID..."
kill -INT "$LIVE_PID" 2>/dev/null || true

for _ in $(seq 1 150); do
    if ! kill -0 "$LIVE_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if kill -0 "$LIVE_PID" 2>/dev/null; then
    echo "[BENCH] WARN: live did not exit in 15s, sending SIGKILL"
    kill -KILL "$LIVE_PID" 2>/dev/null || true
fi

live_exit_code=0
wait "$LIVE_PID" 2>/dev/null || live_exit_code=$?
if [[ $live_exit_code -ne 0 && $live_exit_code -ne 130 && $live_exit_code -ne 137 ]]; then
    echo "[BENCH] FAIL: unexpected live exit code: $live_exit_code" >&2
    tail -n 30 "$LIVE_LOG" >&2 || true
    exit 1
fi

E2E_RESULT="SKIP"
if [[ $RUN_E2E -eq 1 ]]; then
    echo "[BENCH] Running replay count check..."
    if bash "$ROOT/tests/e2e.sh" --skip-live >/tmp/market_data_bench_e2e.log 2>&1; then
        E2E_RESULT="PASS"
    else
        E2E_RESULT="FAIL"
        echo "[BENCH] WARN: e2e failed. Tail:" >&2
        tail -n 30 /tmp/market_data_bench_e2e.log >&2 || true
    fi
fi

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$ROOT/build/CMakeCache.txt" 2>/dev/null | head -n 1 || true)"
if [[ -z "$BUILD_TYPE" ]]; then
    BUILD_TYPE="unknown"
fi

python3 - "$LIVE_LOG" "$SUMMARY_MD" "$ROW_MD" "$DURATION" "$COMMIT" "$BUILD_TYPE" "$E2E_RESULT" <<'PY'
import datetime as dt
import re
import statistics as stats
import sys
from pathlib import Path

log_path, summary_path, row_path, duration, commit, build_type, e2e = sys.argv[1:]
text = Path(log_path).read_text(errors="replace").splitlines()

metric_re = re.compile(r"([a-zA-Z_/]+)=([a-zA-Z0-9_.-]+)")
lat_int_re = re.compile(r"\[LATENCY_INT\] pipeline count=(\d+) p50=(\d+)us p95=(\d+)us p99=(\d+)us max=(\d+)us")
lat_ext_re = re.compile(r"\[LATENCY_EXT\] trade count=(\d+) p50=(\d+)us p95=(\d+)us p99=(\d+)us max=(\d+)us")

metrics = []
lat_int = []
lat_ext = []

for line in text:
    if line.startswith("[METRICS]"):
        fields = dict(metric_re.findall(line))
        def as_int(key, default=0):
            try:
                return int(fields.get(key, default))
            except ValueError:
                return default
        metrics.append({
            "msgs": as_int("msgs/s"),
            "trades": as_int("trades/s"),
            "books": as_int("books/s"),
            "trade_dropped": as_int("trade_dropped/s"),
            "book_dropped": as_int("book_dropped/s"),
            "trade_writer_error": fields.get("trade_writer_error", "unknown"),
            "book_writer_error": fields.get("book_writer_error", "unknown"),
        })
    else:
        m = lat_int_re.search(line)
        if m:
            lat_int.append(tuple(map(int, m.groups())))
            continue
        m = lat_ext_re.search(line)
        if m:
            lat_ext.append(tuple(map(int, m.groups())))

def avg(xs):
    return round(sum(xs) / len(xs)) if xs else 0

def max_or_zero(xs):
    return max(xs) if xs else 0

def rng(xs):
    return f"{min(xs)}-{max(xs)}" if xs else "-"

msg_vals = [m["msgs"] for m in metrics]
trade_vals = [m["trades"] for m in metrics]
book_vals = [m["books"] for m in metrics]
drop_vals = [m["trade_dropped"] + m["book_dropped"] for m in metrics]

int_counts = [x[0] for x in lat_int]
int_p50 = [x[1] for x in lat_int if x[0] > 0]
int_p95 = [x[2] for x in lat_int if x[0] > 0]
int_p99 = [x[3] for x in lat_int if x[0] > 0]
int_max = [x[4] for x in lat_int if x[0] > 0]

ext_counts = [x[0] for x in lat_ext]
ext_nonzero = [x for x in lat_ext if x[0] > 0]

writer_error = any(
    m["trade_writer_error"] == "yes" or m["book_writer_error"] == "yes"
    for m in metrics
)

today = dt.date.today().isoformat()
row = (
    f"| {today} | 自动 benchmark | `{commit}` | {build_type} | {duration}s | "
    f"avg={avg(msg_vals)} max={max_or_zero(msg_vals)} | "
    f"avg={avg(trade_vals)} max={max_or_zero(trade_vals)} | "
    f"avg={avg(book_vals)} max={max_or_zero(book_vals)} | "
    f"avg={avg(int_p50)}us max={max_or_zero(int_p50)}us | "
    f"avg={avg(int_p95)}us max={max_or_zero(int_p95)}us | "
    f"avg={avg(int_p99)}us max={max_or_zero(int_p99)}us | "
    f"{sum(drop_vals)} | {e2e} | "
    f"metrics_windows={len(metrics)}, int_windows={len(lat_int)}, ext_nonzero_windows={len(ext_nonzero)}, writer_error={'yes' if writer_error else 'no'} |"
)

summary = f"""# Benchmark Summary

- Date: {today}
- Commit: `{commit}`
- Build: {build_type}
- Duration: {duration}s
- Log: `{log_path}`
- Metrics windows: {len(metrics)}
- LATENCY_INT windows: {len(lat_int)}
- LATENCY_EXT nonzero windows: {len(ext_nonzero)}
- E2E: {e2e}

## Throughput

| Metric | avg | min-max |
|---|---:|---:|
| msgs/s | {avg(msg_vals)} | {rng(msg_vals)} |
| trades/s | {avg(trade_vals)} | {rng(trade_vals)} |
| books/s | {avg(book_vals)} | {rng(book_vals)} |
| dropped/s(total) | {avg(drop_vals)} | {rng(drop_vals)} |

## LATENCY_INT pipeline

| Metric | avg | max-observed |
|---|---:|---:|
| count/window | {avg(int_counts)} | {max_or_zero(int_counts)} |
| p50 | {avg(int_p50)}us | {max_or_zero(int_p50)}us |
| p95 | {avg(int_p95)}us | {max_or_zero(int_p95)}us |
| p99 | {avg(int_p99)}us | {max_or_zero(int_p99)}us |
| max | {avg(int_max)}us | {max_or_zero(int_max)}us |

## LATENCY_EXT trade

- nonzero windows: {len(ext_nonzero)} / {len(lat_ext)}
- 说明：如果 nonzero windows 很少，通常是本机时间与交易所时间未校准导致负样本被丢弃。

## Storage / Errors

- total dropped samples from metric windows: {sum(drop_vals)}
- writer_error: {'yes' if writer_error else 'no'}

## BENCHMARKS.md row

```markdown
{row}
```
"""

Path(summary_path).write_text(summary)
Path(row_path).write_text(row + "\n")

print(summary)
PY

echo "[BENCH] Summary written to $SUMMARY_MD"
echo "[BENCH] Row written to $ROW_MD"
