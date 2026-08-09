#!/usr/bin/env bash
# 端到端整合测试：live 接收 N 秒 → SIGINT 退出 → --replay 回放 → 比对计数
#
# 校验思路：
#   写出去多少（按文件大小反推：trades.bin = 24B header + 56B * N，
#   orderbooks.bin = 24B header + 664B * M）
#   == 读回来多少（replay 输出里的 replayed=N）
# 不依赖 reporter_loop 的 stdout 格式，因为 reporter 在 g_running=false 时
# 就退出了，writer 在 close 阶段写完队列剩余记录后没有再次打印总数。
#
# 用法：
#   bash tests/e2e.sh                    # 默认 live 30 秒
#   bash tests/e2e.sh --duration 60      # live 60 秒
#   bash tests/e2e.sh --skip-live        # 跳过 live 阶段，复用现有 data/
#   BUILD_DIR=build-cli bash tests/e2e.sh
#   bash tests/e2e.sh --build-dir build-cli

set -euo pipefail

DURATION=30
SKIP_LIVE=0
BUILD_DIR="${BUILD_DIR:-build}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            # 非数字（"30s" / "abc"）会让后面的 $((DURATION - 2)) 直接抛
            # bash 算术错误，set -e 会立刻退出，用户只看到一句不知所云的报错。
            # 在这里就挡掉，给出明确提示。${2:-} 是为了在 set -u 下不触发
            # "unbound variable"（用户传 `--duration` 不带值的情况）。
            if [[ ! "${2:-}" =~ ^[0-9]+$ ]]; then
                echo "[E2E] FAIL: --duration requires a non-negative integer, got '${2:-}'" >&2
                exit 1
            fi
            # 下面 sleep "$((DURATION - 2))"，DURATION<3 会让 sleep 收到 0 或负数：
            #   - 0 是合法的，但 live 实际只跑了启动等待的 2 秒，必然写不出数据
            #   - 负数 GNU sleep 会报 "invalid time interval"
            # 两种都会污染后续断言。直接挡 3 秒下限。
            if (( $2 < 3 )); then
                echo "[E2E] FAIL: --duration must be >= 3 seconds (got $2)" >&2
                exit 1
            fi
            DURATION="$2"
            shift 2
            ;;
        --skip-live)
            SKIP_LIVE=1
            shift
            ;;
        --build-dir)
            if [[ -z "${2:-}" ]]; then
                echo "[E2E] FAIL: --build-dir requires a path" >&2
                exit 1
            fi
            BUILD_DIR="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,18p' "$0"
            exit 0
            ;;
        *)
            echo "[E2E] Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 相对路径按仓库根解析，便于 BUILD_DIR=build / build-cli
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$ROOT/$BUILD_DIR"
fi
BIN="$BUILD_DIR/market-data-system"
DATA_DIR="$ROOT/data"
LIVE_LOG="$BUILD_DIR/e2e_live.log"
REPLAY_LOG="$BUILD_DIR/e2e_replay.log"

# 与 src/storage/trade_file_format.h 中的 Header / 结构体大小保持一致：
#   - TradeFileHeader / OrderBookFileHeader：8B magic + 4B version + 4B record_size + 8B record_count = 24
#   - sizeof(Trade) = 56；sizeof(OrderBookSnapshot) = 664
HEADER_SIZE=24
TRADE_SIZE=56
BOOK_SIZE=664

mkdir -p "$BUILD_DIR"

if [[ ! -x "$BIN" ]]; then
    echo "[E2E] FAIL: binary not found at $BIN. Build that directory first (e.g. cmake --build build)." >&2
    exit 1
fi

if [[ $SKIP_LIVE -eq 0 ]]; then
    echo "[E2E] Cleaning previous data files..."
    rm -f "$DATA_DIR/trades.bin" "$DATA_DIR/orderbooks.bin"

    echo "[E2E] Starting live mode for ${DURATION}s (log: $LIVE_LOG)..."
    cd "$ROOT"
    "$BIN" >"$LIVE_LOG" 2>&1 &
    LIVE_PID=$!

    # 给一点启动时间，避免 SIGINT 来得比 connect 还早
    sleep 2
    if ! kill -0 "$LIVE_PID" 2>/dev/null; then
        echo "[E2E] FAIL: live process exited prematurely. Tail of log:" >&2
        tail -n 30 "$LIVE_LOG" >&2
        exit 1
    fi

    sleep "$((DURATION - 2))"
    # 如果 live 在主采样窗口中途崩了（段错误/abort/配置错误退出），这里要立刻 fail。
    # 否则后面的 kill -INT / wait 都会被 || true 吞掉，可能出现“假绿 PASS”。
    if ! kill -0 "$LIVE_PID" 2>/dev/null; then
        echo "[E2E] FAIL: live process exited during run window. Tail of log:" >&2
        tail -n 30 "$LIVE_LOG" >&2 || true
        wait "$LIVE_PID" 2>/dev/null || true
        exit 1
    fi

    echo "[E2E] Sending SIGINT to PID $LIVE_PID for graceful shutdown..."
    kill -INT "$LIVE_PID" 2>/dev/null || true

    # 等优雅退出。设上限 15s，避免 stop() 卡住时脚本永久挂起。
    for _ in $(seq 1 150); do
        if ! kill -0 "$LIVE_PID" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$LIVE_PID" 2>/dev/null; then
        echo "[E2E] WARN: live did not exit in 15s, sending SIGKILL"
        kill -KILL "$LIVE_PID" 2>/dev/null || true
    fi
    live_exit_code=0
    wait "$LIVE_PID" 2>/dev/null || live_exit_code=$?
    # 130(SIGINT) 表示优雅中断；137(SIGKILL) 表示超时兜底强杀。
    # 其余值都视为 live 异常退出（崩溃/启动失败/配置错误等），必须 fail，避免假绿。
    if [[ $live_exit_code -ne 0 && $live_exit_code -ne 130 && $live_exit_code -ne 137 ]]; then
        echo "[E2E] FAIL: unexpected live exit code: $live_exit_code" >&2
        tail -n 30 "$LIVE_LOG" >&2 || true
        exit 1
    fi
    echo "[E2E] Live mode finished."
fi

if [[ ! -f "$DATA_DIR/trades.bin" || ! -f "$DATA_DIR/orderbooks.bin" ]]; then
    echo "[E2E] FAIL: expected binary files not found in $DATA_DIR" >&2
    exit 1
fi

trade_size_bytes=$(stat -c%s "$DATA_DIR/trades.bin")
book_size_bytes=$(stat -c%s "$DATA_DIR/orderbooks.bin")

if [[ $trade_size_bytes -lt $HEADER_SIZE || $book_size_bytes -lt $HEADER_SIZE ]]; then
    echo "[E2E] FAIL: file smaller than header size; corrupt write?" >&2
    exit 1
fi

if (( (trade_size_bytes - HEADER_SIZE) % TRADE_SIZE != 0 )); then
    echo "[E2E] FAIL: trades.bin body not divisible by ${TRADE_SIZE} (truncated record)" >&2
    exit 1
fi
if (( (book_size_bytes - HEADER_SIZE) % BOOK_SIZE != 0 )); then
    echo "[E2E] FAIL: orderbooks.bin body not divisible by ${BOOK_SIZE} (truncated record)" >&2
    exit 1
fi

trade_disk=$(( (trade_size_bytes - HEADER_SIZE) / TRADE_SIZE ))
book_disk=$(( (book_size_bytes - HEADER_SIZE) / BOOK_SIZE ))
echo "[E2E] On-disk count: trades=$trade_disk orderbooks=$book_disk"

if [[ $trade_disk -eq 0 && $book_disk -eq 0 ]]; then
    echo "[E2E] FAIL: nothing was written. Network or auth issue?" >&2
    echo "[E2E] Tail of $LIVE_LOG:" >&2
    tail -n 30 "$LIVE_LOG" >&2 || true
    exit 1
fi

echo "[E2E] Running replay (log: $REPLAY_LOG)..."
cd "$ROOT"
if ! "$BIN" --replay >"$REPLAY_LOG" 2>&1; then
    echo "[E2E] FAIL: replay exited non-zero" >&2
    tail -n 30 "$REPLAY_LOG" >&2
    exit 1
fi

trade_replayed=$(grep -m1 "Trade summary" "$REPLAY_LOG" \
    | sed -nE 's/.*replayed=([0-9]+).*/\1/p')
book_replayed=$(grep -m1 "OrderBook summary" "$REPLAY_LOG" \
    | sed -nE 's/.*replayed=([0-9]+).*/\1/p')

if [[ -z "$trade_replayed" ]]; then
    trade_replayed="<missing>"
fi
if [[ -z "$book_replayed" ]]; then
    book_replayed="<missing>"
fi

trade_completed=no
book_completed=no
if grep -qE 'Trade summary.*completed=yes' "$REPLAY_LOG"; then
    trade_completed=yes
fi
if grep -qE 'OrderBook summary.*completed=yes' "$REPLAY_LOG"; then
    book_completed=yes
fi

echo "[E2E] Replayed:    trades=${trade_replayed} (completed=${trade_completed})"
echo "[E2E]              orderbooks=${book_replayed} (completed=${book_completed})"

PASS=1
if [[ "$trade_replayed" != "$trade_disk" ]]; then
    echo "[E2E] FAIL: trade count mismatch (disk=$trade_disk replayed=$trade_replayed)" >&2
    PASS=0
fi
if [[ "$book_replayed" != "$book_disk" ]]; then
    echo "[E2E] FAIL: orderbook count mismatch (disk=$book_disk replayed=$book_replayed)" >&2
    PASS=0
fi
if [[ "$trade_completed" != "yes" ]]; then
    echo "[E2E] FAIL: trade replay completed=no" >&2
    PASS=0
fi
if [[ "$book_completed" != "yes" ]]; then
    echo "[E2E] FAIL: orderbook replay completed=no" >&2
    PASS=0
fi

if [[ $PASS -eq 1 ]]; then
    echo "[E2E] PASS"
    exit 0
fi
echo "[E2E] See $LIVE_LOG and $REPLAY_LOG for details" >&2
exit 1
