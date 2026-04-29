/**
 * @file main.cpp
 * @brief 高性能行情数据系统入口（live + replay 双模式）
 *
 * 系统功能：
 * 1. live 模式（默认）：从 Binance WebSocket 接入实时行情 → 缓存 → 二进制落盘
 * 2. replay 模式（--replay）：从磁盘读取 trades.bin / orderbooks.bin 顺序回放
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

#include "cache/order_book_ring_buffer.h"
#include "cache/trade_ring_buffer.h"
#include "config/config.h"
#include "monitor/metrics.h"
#include "network/connection_manager.h"
#include "replay/order_book_replayer.h"
#include "replay/trade_replayer.h"
#include "storage/binary_order_book_writer.h"
#include "storage/binary_trade_writer.h"

std::atomic<bool> g_running{true};
std::atomic<int>  g_signal{0};
std::mutex        g_console_mutex;

// signal_handler 必须 async-signal-safe：只允许 lock-free atomic 操作。
// 之前用 std::cout 是 UB（信号在主线程持 cout 锁时打断 → 同线程二次加锁）。
// 现在只记录 signum + 翻转 g_running，把打印推迟到主循环。
void signal_handler(int signum) {
    g_signal.store(signum, std::memory_order_relaxed);
    g_running.store(false, std::memory_order_relaxed);
}

// 监控上报线程：每秒打印一次"瞬时速率 + 累计值"
//
// 退出拆成 10 次 100ms 小睡眠，避免关机时最多卡 1 秒。
// 不在 signal_handler 里 notify cv，因为那不是 async-signal-safe。
// 注意：metrics 是非 const 引用——LatencyHistogram::snapshot_and_reset()
// 会清零所有桶（这是设计：每秒拿到的是"上一秒的分位数"，不是累计直方图）。
// 其他普通 atomic 计数器仍按 load() 读取，行为不变。
void reporter_loop(mds::Metrics& metrics,
                   const mds::BinaryTradeWriter& trade_writer,
                   const mds::BinaryOrderBookWriter& orderbook_writer) {
    uint64_t prev_msgs = 0, prev_trades = 0, prev_books = 0;
    uint64_t prev_parse_err = 0, prev_cb_err = 0;
    uint64_t prev_trade_written = 0, prev_trade_dropped = 0;
    uint64_t prev_book_written = 0, prev_book_dropped = 0;

    while (g_running.load()) {
        for (int i = 0; i < 10 && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_running.load()) break;

        const uint64_t msgs    = metrics.msgs_recv.load(std::memory_order_relaxed);
        const uint64_t trades  = metrics.trades_parsed.load(std::memory_order_relaxed);
        const uint64_t books   = metrics.orderbooks_parsed.load(std::memory_order_relaxed);
        const uint64_t pe      = metrics.parse_errors.load(std::memory_order_relaxed);
        const uint64_t ce      = metrics.callback_errors.load(std::memory_order_relaxed);
        const uint64_t conn_ok = metrics.connect_successes.load(std::memory_order_relaxed);
        const uint64_t conn_at = metrics.connect_attempts.load(std::memory_order_relaxed);
        const uint64_t trade_written = trade_writer.records_written();
        const uint64_t trade_dropped = trade_writer.records_dropped();
        const uint64_t book_written  = orderbook_writer.records_written();
        const uint64_t book_dropped  = orderbook_writer.records_dropped();

        // 取一次 trade 端到端延迟分位数（顺带清零桶，下一秒重新统计）
        const auto trade_lat = metrics.trade_latency.snapshot_and_reset();

        {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cout << "[METRICS]"
                      << " msgs/s=" << (msgs - prev_msgs)
                      << " trades/s=" << (trades - prev_trades)
                      << " books/s=" << (books - prev_books)
                      << " trade_written/s=" << (trade_written - prev_trade_written)
                      << " trade_dropped/s=" << (trade_dropped - prev_trade_dropped)
                      << " book_written/s=" << (book_written - prev_book_written)
                      << " book_dropped/s=" << (book_dropped - prev_book_dropped)
                      << " parse_err/s=" << (pe - prev_parse_err)
                      << " cb_err/s=" << (ce - prev_cb_err)
                      << " | conn=" << conn_ok << "/" << conn_at
                      << " trade_writer_error=" << (trade_writer.has_error() ? "yes" : "no")
                      << " book_writer_error=" << (orderbook_writer.has_error() ? "yes" : "no")
                      << " | total: msgs=" << msgs
                      << " trades=" << trades << " books=" << books
                      << " trade_written=" << trade_written
                      << " trade_dropped=" << trade_dropped
                      << " book_written=" << book_written
                      << " book_dropped=" << book_dropped
                      << std::endl;
        }

        // 单独一行延迟分位数：count=0 时跳过（reporter 启动后第一秒可能没数据）
        if (trade_lat.count > 0) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cout << "[LATENCY] trade"
                      << " count=" << trade_lat.count
                      << " p50="   << trade_lat.p50_us << "us"
                      << " p95="   << trade_lat.p95_us << "us"
                      << " p99="   << trade_lat.p99_us << "us"
                      << " max="   << trade_lat.max_us << "us"
                      << std::endl;
        }

        prev_msgs = msgs;
        prev_trades = trades;
        prev_books = books;
        prev_parse_err = pe;
        prev_cb_err = ce;
        prev_trade_written = trade_written;
        prev_trade_dropped = trade_dropped;
        prev_book_written = book_written;
        prev_book_dropped = book_dropped;
    }
}

// live 模式：实时接收 + 落盘
//
// 声明顺序约束（析构反向）：metrics / writers / caches 必须在 mgr 之前声明，
// 这样 mgr 先析构（stop + join 回调线程），之后 cache/writer/metrics 再析构，
// 回调 lambda 捕获的引用始终有效。
int run_live(const mds::Config& config) {
    // 进入 live 之前先把"必填配置"挡掉。放在 writer.open 之前，避免无谓地
    // 创建空的 trades.bin / orderbooks.bin。
    // 注意：默认 Config 自带一个 binance_main 数据源，所以正常情况下 size>=1；
    // 这里挡的是用户在 config.json 里显式写 "data_sources": [] 的边界情况。
    if (config.data_sources.empty()) {
        std::cerr << "[ERROR] No data_sources configured" << std::endl;
        return 1;
    }
    const auto& source = config.data_sources[0];
    if (source.ws_url.empty()) {
        std::cerr << "[ERROR] First data_source has empty ws_url" << std::endl;
        return 1;
    }

    mds::Metrics metrics;

    mds::BinaryTradeWriter trade_writer;
    if (!trade_writer.open(config.data_dir, config.ring_buffer_size)) {
        return 1;
    }
    mds::BinaryOrderBookWriter orderbook_writer;
    if (!orderbook_writer.open(config.data_dir, config.ring_buffer_size)) {
        return 1;
    }

    mds::TradeRingBuffer     trade_cache(config.ring_buffer_size);
    mds::OrderBookRingBuffer orderbook_cache(config.ring_buffer_size);

    mds::ConnectionManager mgr(config, metrics);

    // live 明细日志采样：前几条全打，之后每 N 条打一条。
    // 这样保留肉眼 sanity check，又不会把每秒一条的 METRICS/LATENCY 淹没。
    constexpr uint64_t LIVE_TRADE_PRINT_EVERY = 1000;
    constexpr uint64_t LIVE_BOOK_PRINT_EVERY  = 1000;
    constexpr uint64_t LIVE_HEAD_SAMPLE       = 5;
    std::atomic<uint64_t> live_trade_print_count{0};
    std::atomic<uint64_t> live_book_print_count{0};

    mgr.set_trade_callback([&trade_writer, &trade_cache,
                            &live_trade_print_count](const mds::Trade& t) {
        trade_cache.push(t);
        if (!trade_writer.write(t)) {
            std::cerr << "[ERROR] Failed to write trade" << std::endl;
        }

        const uint64_t n = live_trade_print_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= LIVE_HEAD_SAMPLE || n % LIVE_TRADE_PRINT_EVERY == 0) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cout << "[TRADE] #" << n << " " << t.symbol
                      << " price=" << t.price
                      << " qty=" << t.quantity << std::endl;
        }
    });

    mgr.set_orderbook_callback([&orderbook_cache, &orderbook_writer,
                                &live_book_print_count](
                                   const mds::OrderBookSnapshot& ob) {
        orderbook_cache.push(ob);
        if (!orderbook_writer.write(ob)) {
            std::cerr << "[ERROR] Failed to write orderbook" << std::endl;
        }

        const uint64_t n = live_book_print_count.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= LIVE_HEAD_SAMPLE || n % LIVE_BOOK_PRINT_EVERY == 0) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cout << "[BOOK]  #" << n << " " << ob.symbol
                      << " bid=" << ob.best_bid_price()
                      << " ask=" << ob.best_ask_price()
                      << " spread=" << ob.spread() << std::endl;
        }
    });

    mgr.start(source.ws_url);

    // metrics 用 std::ref（非 const）：reporter 需要 snapshot_and_reset
    // 把延迟桶清零；其余两个 writer 仍是只读 cref。
    std::thread reporter_thread(reporter_loop, std::ref(metrics),
                                std::cref(trade_writer), std::cref(orderbook_writer));

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (const int sig = g_signal.load(std::memory_order_relaxed); sig != 0) {
        std::cout << "\n[INFO] Received signal " << sig << ", shutting down..." << std::endl;
    }

    mgr.stop();
    if (reporter_thread.joinable()) {
        reporter_thread.join();
    }
    std::cout << "[INFO] Live mode shutdown complete." << std::endl;
    return 0;
}

// replay 模式：从 data_dir 顺序读出 trades.bin / orderbooks.bin
//
// 数据量可能很大（一晚上几十万条），全打 stdout 会刷屏，所以每 print_every 条
// 才打一行；前 5 条总是打，便于人工肉眼快速 sanity check。
//
// 退出码：两个文件都 completed → 0；任一损坏或 callback 出错 → 1。
int run_replay(const mds::Config& config) {
    std::cout << "[REPLAY] Reading from " << config.data_dir << std::endl;

    constexpr uint64_t TRADE_PRINT_EVERY = 10000;
    constexpr uint64_t BOOK_PRINT_EVERY  = 1000;
    constexpr uint64_t HEAD_SAMPLE       = 5;

    uint64_t trade_cnt = 0;
    mds::TradeReplayer trade_replayer;
    auto trade_result = trade_replayer.replay_all(
        config.data_dir,
        [&trade_cnt](const mds::Trade& t) {
            ++trade_cnt;
            if (trade_cnt <= HEAD_SAMPLE || trade_cnt % TRADE_PRINT_EVERY == 0) {
                std::cout << "[REPLAY-TRADE] #" << trade_cnt
                          << " ts=" << t.timestamp_us
                          << " " << t.symbol
                          << " price=" << t.price
                          << " qty=" << t.quantity << std::endl;
            }
        });
    std::cout << "[REPLAY] Trade summary:"
              << " replayed=" << trade_result.records_replayed
              << " completed=" << (trade_result.completed ? "yes" : "no")
              << " file_error=" << (trade_result.file_error ? "yes" : "no")
              << " callback_error=" << (trade_result.callback_error ? "yes" : "no")
              << std::endl;

    uint64_t book_cnt = 0;
    mds::OrderBookReplayer ob_replayer;
    auto ob_result = ob_replayer.replay_all(
        config.data_dir,
        [&book_cnt](const mds::OrderBookSnapshot& ob) {
            ++book_cnt;
            if (book_cnt <= HEAD_SAMPLE || book_cnt % BOOK_PRINT_EVERY == 0) {
                std::cout << "[REPLAY-BOOK] #" << book_cnt
                          << " ts=" << ob.timestamp_us
                          << " " << ob.symbol
                          << " bid=" << ob.best_bid_price()
                          << " ask=" << ob.best_ask_price()
                          << " spread=" << ob.spread() << std::endl;
            }
        });
    std::cout << "[REPLAY] OrderBook summary:"
              << " replayed=" << ob_result.records_replayed
              << " completed=" << (ob_result.completed ? "yes" : "no")
              << " file_error=" << (ob_result.file_error ? "yes" : "no")
              << " callback_error=" << (ob_result.callback_error ? "yes" : "no")
              << std::endl;

    const bool ok = trade_result.completed && ob_result.completed;
    return ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    bool replay_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--replay") {
            replay_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--replay]\n"
                      << "  (no args)  Live mode: receive market data and persist to disk\n"
                      << "  --replay   Replay mode: read persisted data and dump to stdout\n";
            return 0;
        } else {
            std::cerr << "[ERROR] Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    // 只有 live 模式注册信号处理器：live 跑的是无限接收循环，必须靠
    // SIGINT/SIGTERM 翻 g_running 才能优雅退出。
    // replay 模式刻意不注册：RecordReplayer::replay_all 是不可中断的同步循环，
    // 注册了 handler 反而会把 Ctrl+C 吞掉，让用户以为程序卡死。
    // 让默认 SIG_DFL 直接终止进程，体感更好。等到日后回放支持中断时再注册回来。
    if (!replay_mode) {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
    }

    std::cout << "==================================" << std::endl;
    std::cout << " Market Data System v0.1.0 ("
              << (replay_mode ? "replay" : "live") << ")" << std::endl;
    std::cout << "==================================" << std::endl;

    // config.load 内部会抛 std::runtime_error / std::invalid_argument
    // （比如 ring_buffer_size 类型/范围非法、proxy_url 类型不对等）。
    // 这里统一接住，转成"打印 + 非零退出码"，避免抛到 main 外面变成 terminate。
    mds::Config config;
    try {
        config.load("config.json");
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load config: " << e.what() << std::endl;
        return 1;
    }

    return replay_mode ? run_replay(config) : run_live(config);
}
