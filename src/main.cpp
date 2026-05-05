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
#include <cctype>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "benchmark/pipeline_benchmark.h"
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

namespace {

// 延迟直方图按较长窗口汇总再 snapshot，避免“每秒只有几十条样本”时 P99 抖动和
// 与离线百万条窗口的 P99 完全不可比。吞吐类 METRICS 仍保持 1s 粒度。
constexpr int kLatencyReportIntervalSeconds = 30;

void print_latency_lines(const mds::LatencyHistogram::Snapshot& trade_lat,
                         const mds::LatencyHistogram::Snapshot& pipeline_lat) {
    std::lock_guard<std::mutex> lock(g_console_mutex);
    std::cout << "[LATENCY_EXT] trade"
              << " count=" << trade_lat.count
              << " p50="   << trade_lat.p50_us << "us"
              << " p95="   << trade_lat.p95_us << "us"
              << " p99="   << trade_lat.p99_us << "us"
              << " max="   << trade_lat.max_us << "us"
              << std::endl;
    std::cout << "[LATENCY_INT] pipeline"
              << " count=" << pipeline_lat.count
              << " p50="   << pipeline_lat.p50_us << "us"
              << " p95="   << pipeline_lat.p95_us << "us"
              << " p99="   << pipeline_lat.p99_us << "us"
              << " max="   << pipeline_lat.max_us << "us"
              << std::endl;
}

}  // namespace

// 监控上报线程：每秒打印一次"瞬时速率 + 累计值"；延迟分位数每
// kLatencyReportIntervalSeconds 秒打印并重置直方图（样本更多、P99 更稳）。
//
// 退出拆成 10 次 100ms 小睡眠，避免关机时最多卡 1 秒。
// 不在 signal_handler 里 notify cv，因为那不是 async-signal-safe。
// 注意：metrics 是非 const 引用——仅在延迟上报 tick 上调用
// LatencyHistogram::snapshot_and_reset()；其它 atomic 计数器仍按 load() 读取。
void reporter_loop(mds::Metrics& metrics,
                   const mds::BinaryTradeWriter& trade_writer,
                   const mds::BinaryOrderBookWriter& orderbook_writer) {
    uint64_t prev_msgs = 0, prev_trades = 0, prev_books = 0;
    uint64_t prev_parse_err = 0, prev_cb_err = 0;
    uint64_t prev_trade_written = 0, prev_trade_dropped = 0;
    uint64_t prev_book_written = 0, prev_book_dropped = 0;
    uint64_t seconds_tick           = 0;

    while (g_running.load()) {
        for (int i = 0; i < 10 && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_running.load()) break;

        ++seconds_tick;
        const bool report_latency_window =
            (seconds_tick % kLatencyReportIntervalSeconds == 0);

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

        if (report_latency_window) {
            // EXT：交易所时间戳 -> 本机回调到达，包含公网/代理。
            // INT：on_raw_message 进入 -> 函数退出，只看本进程内部处理。
            const auto trade_lat     = metrics.trade_latency.snapshot_and_reset();
            const auto pipeline_lat  = metrics.pipeline_latency.snapshot_and_reset();
            print_latency_lines(trade_lat, pipeline_lat);
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

    // 退出时 flush 未满一个上报窗口的延迟样本，避免“最后一截”只存在于内存里。
    const auto trade_lat_final    = metrics.trade_latency.snapshot_and_reset();
    const auto pipeline_lat_final = metrics.pipeline_latency.snapshot_and_reset();
    if (trade_lat_final.count > 0 || pipeline_lat_final.count > 0) {
        print_latency_lines(trade_lat_final, pipeline_lat_final);
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

int run_bench_pipeline(const mds::Config& config, uint64_t messages, bool enable_write,
                       uint64_t message_gap_us) {
    mds::PipelineBenchmarkResult result;
    try {
        result = mds::run_pipeline_benchmark(config, messages, enable_write,
                                             message_gap_us);
    } catch (const std::exception& e) {
        std::cerr << "[BENCH-PIPELINE] error=" << e.what() << std::endl;
        return 1;
    }
    std::cout << "[BENCH-PIPELINE]"
              << " messages=" << result.messages
              << " write=" << (enable_write ? "yes" : "no")
              << " gap_us=" << message_gap_us
              << " processing_seconds=" << result.processing_seconds
              << " total_seconds=" << result.total_seconds
              << " processing_msgs/s="
              << static_cast<uint64_t>(result.processing_msgs_per_sec)
              << " total_msgs/s=" << static_cast<uint64_t>(result.total_msgs_per_sec)
              << " trades=" << result.trades
              << "/" << result.expected_trades
              << " books=" << result.orderbooks
              << "/" << result.expected_orderbooks
              << " parse_errors=" << result.parse_errors
              << " callback_errors=" << result.callback_errors
              << " validation_errors=" << result.validation_errors
              << " trade_written=" << result.trade_written
              << " trade_dropped=" << result.trade_dropped
              << " book_written=" << result.orderbook_written
              << " book_dropped=" << result.orderbook_dropped
              << " writer_error=" << (result.writer_error ? "yes" : "no")
              << " p50=" << result.p50_us << "us"
              << " p95=" << result.p95_us << "us"
              << " p99=" << result.p99_us << "us"
              << " max=" << result.max_us << "us"
              << std::endl;
    const bool ok = result.parse_errors == 0 &&
                    result.callback_errors == 0 &&
                    result.validation_errors == 0 &&
                    !result.writer_error &&
                    result.trade_dropped == 0 &&
                    result.orderbook_dropped == 0 &&
                    result.trades == result.expected_trades &&
                    result.orderbooks == result.expected_orderbooks &&
                    (!enable_write ||
                     (result.trade_written == result.expected_trades &&
                      result.orderbook_written == result.expected_orderbooks));
    return ok ? 0 : 1;
}

enum class AppMode {
    Live,
    Replay,
    BenchPipeline,
};

struct CliOptions {
    AppMode mode = AppMode::Live;
    uint64_t bench_messages = 100000;
    uint64_t bench_gap_us = 0;
    bool bench_gap_specified = false;
    bool bench_write = false;
    bool show_help = false;
};

const char* mode_name(AppMode mode) {
    switch (mode) {
        case AppMode::Live:
            return "live";
        case AppMode::Replay:
            return "replay";
        case AppMode::BenchPipeline:
            return "bench-pipeline";
    }
    return "unknown";
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [--replay] [--bench-pipeline [messages]] [--bench-write]"
              << " [--bench-gap-us <us>]\n"
              << "  (no args)                 Live mode: receive market data and persist to disk\n"
              << "  --replay                  Replay mode: read persisted data and dump to stdout\n"
              << "  --bench-pipeline [N]      Offline parser/callback benchmark with generated JSON\n"
              << "  --bench-write             Include cache + binary writers in --bench-pipeline\n"
              << "  --bench-gap-us <us>       Sleep between benchmark messages (simulate arrival pacing)\n";
}

bool parse_u64_with_min(const std::string& value, uint64_t min_value,
                        uint64_t& out, std::string& error) {
    if (value.empty()) {
        error = "value must not be empty";
        return false;
    }
    if (value[0] == '-') {
        error = "negative value is not allowed";
        return false;
    }
    for (const unsigned char ch : value) {
        if (std::isspace(ch)) {
            error = "whitespace is not allowed";
            return false;
        }
    }
    try {
        size_t pos = 0;
        out = std::stoull(value, &pos);
        if (pos != value.size()) {
            error = "trailing characters in integer: " + value;
            return false;
        }
        if (out < min_value) {
            error = "value must be >= " + std::to_string(min_value);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool parse_cli_options(int argc, char* argv[], CliOptions& options, std::string& error) {
    auto select_mode = [&](AppMode mode, std::string_view flag) {
        if (options.mode != AppMode::Live) {
            error = "only one mode flag can be used; duplicate/conflicting flag: ";
            error += flag;
            return false;
        }
        options.mode = mode;
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            return true;
        }
        if (arg == "--replay") {
            if (!select_mode(AppMode::Replay, arg)) {
                return false;
            }
            continue;
        }
        if (arg == "--bench-pipeline") {
            if (!select_mode(AppMode::BenchPipeline, arg)) {
                return false;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string value = argv[++i];
                if (!parse_u64_with_min(value, 1, options.bench_messages, error)) {
                    error = "invalid --bench-pipeline messages: " + error;
                    return false;
                }
            }
            continue;
        }
        if (arg == "--bench-write") {
            options.bench_write = true;
            continue;
        }
        if (arg == "--bench-gap-us") {
            if (i + 1 >= argc) {
                error = "--bench-gap-us requires a non-negative integer";
                return false;
            }
            const std::string value = argv[++i];
            options.bench_gap_specified = true;
            if (!parse_u64_with_min(value, 0, options.bench_gap_us, error)) {
                error = "invalid --bench-gap-us: " + error;
                return false;
            }
            const auto max_gap = static_cast<uint64_t>(
                std::numeric_limits<std::chrono::microseconds::rep>::max());
            if (options.bench_gap_us > max_gap) {
                error = "--bench-gap-us exceeds supported range";
                return false;
            }
            continue;
        }
        error = "unknown argument: ";
        error += arg;
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    CliOptions cli;
    std::string cli_error;
    if (!parse_cli_options(argc, argv, cli, cli_error)) {
        std::cerr << "[ERROR] " << cli_error << std::endl;
        return 1;
    }
    if (cli.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (cli.bench_write && cli.mode != AppMode::BenchPipeline) {
        std::cerr << "[ERROR] --bench-write requires --bench-pipeline" << std::endl;
        return 1;
    }
    if (cli.bench_gap_specified && cli.mode != AppMode::BenchPipeline) {
        std::cerr << "[ERROR] --bench-gap-us requires --bench-pipeline" << std::endl;
        return 1;
    }

    // 只有 live 模式注册信号处理器：live 跑的是无限接收循环，必须靠
    // SIGINT/SIGTERM 翻 g_running 才能优雅退出。
    // replay 模式刻意不注册：RecordReplayer::replay_all 是不可中断的同步循环，
    // 注册了 handler 反而会把 Ctrl+C 吞掉，让用户以为程序卡死。
    // 让默认 SIG_DFL 直接终止进程，体感更好。等到日后回放支持中断时再注册回来。
    if (cli.mode == AppMode::Live) {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
    }

    std::cout << "==================================" << std::endl;
    std::cout << " Market Data System v0.1.0 (" << mode_name(cli.mode) << ")"
              << std::endl;
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

    if (cli.mode == AppMode::BenchPipeline) {
        return run_bench_pipeline(config, cli.bench_messages, cli.bench_write,
                                  cli.bench_gap_us);
    }
    return cli.mode == AppMode::Replay ? run_replay(config) : run_live(config);
}
