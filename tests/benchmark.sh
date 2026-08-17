#!/usr/bin/env bash
# 性能测试脚本。
#
# 默认走离线基线（--bench-pipeline），消息由 pipeline_benchmark 确定性合成，
# 同一 commit 多次复跑结果应接近（见 BENCHMARKS.md「离线基线」）。
#
# 用法：
#   bash tests/benchmark.sh                          # 离线基线（默认 50000 msgs, --no-write）
#   bash tests/benchmark.sh --messages 50000 --gap-us 0 --no-write
#   bash tests/benchmark.sh --runs 3                 # 连跑 3 次并检查 P99 波动
#   bash tests/benchmark.sh --bench-write            # 显式打开写盘（与默认基线不可直接比）
#   bash tests/benchmark.sh --live --duration 60     # 旧 live 行情（仅参考，不可比）
#   bash tests/benchmark.sh --live --strict-e2e      # live 后 e2e 失败则 exit 1
#   BUILD_DIR=build-cli bash tests/benchmark.sh
#
# 离线输出：
#   - $BUILD_DIR/benchmark_offline.log
#   - $BUILD_DIR/benchmark_offline_summary.md
#   - $BUILD_DIR/benchmark_offline_row.md（对齐 BENCHMARKS.md 离线表）

set -euo pipefail

MODE=offline
DURATION=60
RUN_E2E=1
STRICT_E2E=0
MESSAGES=50000
GAP_US=0
RUNS=1
# 默认不写盘：离线基线比的是解析/回调路径；写盘会显著拖慢 msgs/s，必须显式 --bench-write
BENCH_WRITE=0
BUILD_DIR="${BUILD_DIR:-build}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --offline)
            MODE=offline
            shift
            ;;
        --live)
            MODE=live
            shift
            ;;
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
        --messages)
            if [[ ! "${2:-}" =~ ^[0-9]+$ ]] || (( $2 < 1 )); then
                echo "[BENCH] FAIL: --messages requires a positive integer, got '${2:-}'" >&2
                exit 1
            fi
            MESSAGES="$2"
            shift 2
            ;;
        --gap-us)
            if [[ ! "${2:-}" =~ ^[0-9]+$ ]]; then
                echo "[BENCH] FAIL: --gap-us requires a non-negative integer, got '${2:-}'" >&2
                exit 1
            fi
            GAP_US="$2"
            shift 2
            ;;
        --runs)
            if [[ ! "${2:-}" =~ ^[0-9]+$ ]] || (( $2 < 1 )); then
                echo "[BENCH] FAIL: --runs requires a positive integer, got '${2:-}'" >&2
                exit 1
            fi
            RUNS="$2"
            shift 2
            ;;
        --no-write)
            BENCH_WRITE=0
            shift
            ;;
        --bench-write)
            BENCH_WRITE=1
            shift
            ;;
        --skip-e2e)
            RUN_E2E=0
            shift
            ;;
        --strict-e2e)
            STRICT_E2E=1
            shift
            ;;
        --build-dir)
            if [[ -z "${2:-}" ]]; then
                echo "[BENCH] FAIL: --build-dir requires a path" >&2
                exit 1
            fi
            BUILD_DIR="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,22p' "$0"
            exit 0
            ;;
        *)
            echo "[BENCH] Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$ROOT/$BUILD_DIR"
fi
BIN="$BUILD_DIR/market-data-system"
DATA_DIR="$ROOT/data"
mkdir -p "$BUILD_DIR"

if [[ ! -x "$BIN" ]]; then
    echo "[BENCH] FAIL: binary not found at $BIN. Build that directory first." >&2
    exit 1
fi

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -n 1 || true)"
# 空字符串时 CMake 实际按 Release 默认（见根 CMakeLists.txt）
if [[ -z "$BUILD_TYPE" ]]; then
    BUILD_TYPE="Release"
fi

run_offline_once() {
    local run_id="$1"
    local log_path="$2"
    local write_args=()
    if [[ $BENCH_WRITE -eq 1 ]]; then
        write_args+=(--bench-write)
    fi

    echo "[BENCH] offline run#$run_id: messages=$MESSAGES gap_us=$GAP_US write=$BENCH_WRITE"
    cd "$ROOT"
    "$BIN" --bench-pipeline "$MESSAGES" --bench-gap-us "$GAP_US" "${write_args[@]}" \
        >"$log_path" 2>&1
}

if [[ "$MODE" == "offline" ]]; then
    OFFLINE_LOG="$BUILD_DIR/benchmark_offline.log"
    SUMMARY_MD="$BUILD_DIR/benchmark_offline_summary.md"
    ROW_MD="$BUILD_DIR/benchmark_offline_row.md"
    RUN_LOGS=()
    P99_LIST=()

    : >"$OFFLINE_LOG"
    for ((i = 1; i <= RUNS; i++)); do
        run_log="$BUILD_DIR/benchmark_offline_run${i}.log"
        run_offline_once "$i" "$run_log"
        {
            echo "===== run $i ====="
            cat "$run_log"
            echo
        } >>"$OFFLINE_LOG"
        RUN_LOGS+=("$run_log")

        p99="$(sed -nE 's/.*\[BENCH-PIPELINE\].* p99=([0-9]+)us.*/\1/p' "$run_log" | head -n 1)"
        if [[ -z "$p99" ]]; then
            echo "[BENCH] FAIL: cannot parse p99 from $run_log" >&2
            tail -n 40 "$run_log" >&2 || true
            exit 1
        fi
        P99_LIST+=("$p99")
    done

    # 多跑时检查 P99 稳定性。
    # 指数桶直方图相邻桶上界相差 2×，小延迟时 32us vs 64us 会算出 100%「波动」，
    # 实际只是分桶量化。规则：允许不超过一个桶阶（hi <= 2*lo）；超出再用 10% 相对差。
    if (( RUNS >= 2 )); then
        python3 - "${P99_LIST[@]}" <<'PY'
import sys
vals = [int(x) for x in sys.argv[1:]]
lo, hi = min(vals), max(vals)
if lo <= 0:
    raise SystemExit(f"[BENCH] FAIL: invalid p99 samples: {vals}")
spread = (hi - lo) / lo
print(f"[BENCH] P99 samples={vals} relative_spread={spread:.4f}")
if hi <= 2 * lo:
    print("[BENCH] P99 within one histogram bucket step; OK")
elif spread >= 0.10:
    raise SystemExit(
        f"[BENCH] FAIL: P99 spread {spread:.2%} >= 10% across runs {vals}"
    )
else:
    print("[BENCH] P99 spread OK (< 10%)")
PY
    fi

    python3 - "$SUMMARY_MD" "$ROW_MD" "$COMMIT" "$BUILD_TYPE" "$MESSAGES" "$GAP_US" \
        "$BENCH_WRITE" "${RUN_LOGS[@]}" <<'PY'
import datetime as dt
import re
import sys
from pathlib import Path

summary_path, row_path, commit, build_type, messages, gap_us, bench_write, *logs = sys.argv[1:]
bench_re = re.compile(
    r"\[BENCH-PIPELINE\].*?"
    r"processing_msgs/s=(\d+).*?"
    r"trade_dropped=(\d+).*?"
    r"book_dropped=(\d+).*?"
    r"p50=(\d+)us.*?p99=(\d+)us.*?p99\.9=(\d+)us.*?max=(\d+)us"
)

rows = []
for path in logs:
    text = Path(path).read_text(errors="replace")
    m = bench_re.search(text.replace("\n", " "))
    if not m:
        raise SystemExit(f"[BENCH] FAIL: cannot parse BENCH-PIPELINE line in {path}")
    msgs_s, td, bd, p50, p99, p999, mx = map(int, m.groups())
    dropped = td + bd
    if dropped != 0:
        raise SystemExit(
            f"[BENCH] FAIL: dropped={dropped} in {path} "
            f"(trade_dropped={td}, book_dropped={bd}); expected 0"
        )
    rows.append({
        "msgs_s": msgs_s,
        "dropped": dropped,
        "p50": p50,
        "p99": p99,
        "p999": p999,
        "max": mx,
    })

def avg(xs):
    return round(sum(xs) / len(xs)) if xs else 0

today = dt.date.today().isoformat()
write_label = "yes" if bench_write == "1" else "no"
msgs_avg = avg([r["msgs_s"] for r in rows])
p50_avg = avg([r["p50"] for r in rows])
p99_avg = avg([r["p99"] for r in rows])
p999_avg = avg([r["p999"] for r in rows])
dropped_sum = sum(r["dropped"] for r in rows)
if dropped_sum != 0:
    raise SystemExit(f"[BENCH] FAIL: aggregate dropped={dropped_sum}, expected 0")
p99_vals = [r["p99"] for r in rows]
p99_rng = f"{min(p99_vals)}-{max(p99_vals)}" if p99_vals else "-"

# 对齐 BENCHMARKS.md「离线基线」表头：
# 日期 | Commit | Build | messages | gap_us | write | msgs/s | p50 | p99 | p99.9 | dropped | 结论
row = (
    f"| {today} | `{commit}` | {build_type} | {messages} | {gap_us} | {write_label} | "
    f"{msgs_avg} | {p50_avg}us | {p99_avg}us | {p999_avg}us | {dropped_sum} | "
    f"offline deterministic; runs={len(rows)}; p99_range={p99_rng} |"
)

summary = f"""# Offline Benchmark Summary

- Date: {today}
- Commit: `{commit}`
- Build: {build_type}
- Mode: offline (`--bench-pipeline`)
- Dataset: deterministic synthetic trade/depth JSON in `pipeline_benchmark.cpp`
- messages: {messages}
- gap_us: {gap_us}
- write: {write_label}
- runs: {len(rows)}

## Aggregates

| Metric | value |
|---|---:|
| msgs/s (avg) | {msgs_avg} |
| p50 (avg) | {p50_avg}us |
| p99 (avg) | {p99_avg}us |
| p99.9 (avg) | {p999_avg}us |
| dropped (sum) | {dropped_sum} |
| p99 range | {p99_rng} |

## Per-run

| run | msgs/s | p50 | p99 | p99.9 | dropped |
|---:|---:|---:|---:|---:|---:|
""" + "\n".join(
    f"| {i+1} | {r['msgs_s']} | {r['p50']}us | {r['p99']}us | {r['p999']}us | {r['dropped']} |"
    for i, r in enumerate(rows)
) + f"""

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
    exit 0
fi

# ---------- live mode（行情波动，仅作参考，不可严格对比） ----------
LIVE_LOG="$BUILD_DIR/benchmark_live.log"
SUMMARY_MD="$BUILD_DIR/benchmark_summary.md"
ROW_MD="$BUILD_DIR/benchmark_row.md"

echo "[BENCH] Cleaning previous data files..."
rm -f "$DATA_DIR"/trades.bin "$DATA_DIR"/orderbooks.bin
rm -f "$DATA_DIR"/trades_*.bin "$DATA_DIR"/orderbooks_*.bin

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
    if bash "$ROOT/tests/e2e.sh" --skip-live --build-dir "$BUILD_DIR" \
        >/tmp/market_data_bench_e2e.log 2>&1; then
        E2E_RESULT="PASS"
    else
        E2E_RESULT="FAIL"
        echo "[BENCH] WARN: e2e failed. Tail:" >&2
        tail -n 30 /tmp/market_data_bench_e2e.log >&2 || true
        if [[ $STRICT_E2E -eq 1 ]]; then
            echo "[BENCH] FAIL: --strict-e2e enabled and e2e failed" >&2
            exit 1
        fi
    fi
fi

python3 - "$LIVE_LOG" "$SUMMARY_MD" "$ROW_MD" "$DURATION" "$COMMIT" "$BUILD_TYPE" "$E2E_RESULT" <<'PY'
import datetime as dt
import re
import sys
from pathlib import Path

log_path, summary_path, row_path, duration, commit, build_type, e2e = sys.argv[1:]
text = Path(log_path).read_text(errors="replace").splitlines()

metric_re = re.compile(r"([a-zA-Z_/]+)=([a-zA-Z0-9_.-]+)")
# live LATENCY_INT 现含 p99.9；兼容旧日志无该字段
lat_int_re = re.compile(
    r"\[LATENCY_INT\] pipeline count=(\d+) p50=(\d+)us p95=(\d+)us p99=(\d+)us"
    r"(?: p99\.9=(\d+)us)? max=(\d+)us"
)

metrics = []
lat_int = []

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
            "trade_dropped": as_int("trade_dropped/s"),
            "book_dropped": as_int("book_dropped/s"),
        })
    else:
        m = lat_int_re.search(line)
        if m:
            count, p50, p95, p99, p999, mx = m.groups()
            lat_int.append((
                int(count), int(p50), int(p95), int(p99),
                int(p999) if p999 is not None else 0,
                int(mx),
            ))

def avg(xs):
    return round(sum(xs) / len(xs)) if xs else 0

def max_or_zero(xs):
    return max(xs) if xs else 0

msg_vals = [m["msgs"] for m in metrics]
drop_vals = [m["trade_dropped"] + m["book_dropped"] for m in metrics]
int_p99 = [x[3] for x in lat_int if x[0] > 0]

today = dt.date.today().isoformat()
# 对齐 BENCHMARKS.md live 表头（10 列）：
# 日期 | 改动 | Commit | 时长 | msgs/s(avg/max) | INT p99(avg/max) | INT p99 avg变化 | dropped | e2e | 结论
row = (
    f"| {today} | live auto benchmark | `{commit}` | {duration}s | "
    f"{avg(msg_vals)} / {max_or_zero(msg_vals)} | "
    f"{avg(int_p99)}us / {max_or_zero(int_p99)}us | "
    f"(fill manually) | {sum(drop_vals)} | {e2e} | live reference only |"
)

summary = f"""# Live Benchmark Summary (reference only)

- Date: {today}
- Commit: `{commit}`
- Build: {build_type}
- Duration: {duration}s
- E2E: {e2e}
- NOTE: live traffic varies; use offline mode for comparable baselines.

## Throughput

- msgs/s avg/max: {avg(msg_vals)} / {max_or_zero(msg_vals)}
- dropped sum: {sum(drop_vals)}
- INT p99 avg/max: {avg(int_p99)}us / {max_or_zero(int_p99)}us

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
