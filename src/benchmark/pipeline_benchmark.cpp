#include "pipeline_benchmark.h"

#include "../cache/order_book_ring_buffer.h"
#include "../cache/trade_ring_buffer.h"
#include "../monitor/metrics.h"
#include "../network/connection_manager.h"
#include "../storage/binary_order_book_writer.h"
#include "../storage/binary_trade_writer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>

namespace mds {

namespace {

std::string upper_symbol(std::string symbol) {
    for (char& ch : symbol) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return symbol;
}

std::string lower_symbol(std::string symbol) {
    for (char& ch : symbol) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return symbol;
}

bool symbol_equals(const char* actual, size_t capacity, const std::string& expected) {
    if (actual == nullptr || expected.size() >= capacity) {
        return false;
    }
    size_t len = 0;
    while (len < capacity && actual[len] != '\0') {
        ++len;
    }
    return len == expected.size() &&
           std::memcmp(actual, expected.data(), expected.size()) == 0;
}

bool is_valid_symbol(const std::string& symbol) {
    constexpr size_t kMaxBenchmarkSymbolLen = 15;
    if (symbol.empty() || symbol.size() > kMaxBenchmarkSymbolLen) {
        return false;
    }
    for (char ch : symbol) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9');
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string make_trade_message(const std::string& symbol) {
    if (!is_valid_symbol(symbol)) {
        throw std::invalid_argument("benchmark symbol must be alphanumeric and <= 15 chars");
    }
    const auto sym_lower = lower_symbol(symbol);
    const auto sym_upper = upper_symbol(symbol);
    return "{\"stream\":\"" + sym_lower + "@trade\",\"data\":{"
           "\"e\":\"trade\",\"E\":1777446266491,\"s\":\"" + sym_upper + "\","
           "\"t\":123456789,\"p\":\"108160.55000000\","
           "\"q\":\"0.00040000\",\"b\":987654321,\"a\":987654322,"
           "\"T\":1777446266488,\"m\":false,\"M\":true}}";
}

std::string make_orderbook_message(const std::string& symbol) {
    static_assert(ORDERBOOK_DEPTH == 20, "synthetic depth stream expects 20 levels");
    if (!is_valid_symbol(symbol)) {
        throw std::invalid_argument("benchmark symbol must be alphanumeric and <= 15 chars");
    }
    const auto sym_lower = lower_symbol(symbol);
    std::ostringstream out;
    out << "{\"stream\":\"" << sym_lower << "@depth20@100ms\",\"data\":{\"lastUpdateId\":1,\"bids\":[";
    for (int i = 0; i < ORDERBOOK_DEPTH; ++i) {
        if (i != 0) out << ',';
        out << "[\"" << (108160 - i) << ".55000000\",\"0.00040000\"]";
    }
    out << "],\"asks\":[";
    for (int i = 0; i < ORDERBOOK_DEPTH; ++i) {
        if (i != 0) out << ',';
        out << "[\"" << (108161 + i) << ".55000000\",\"0.00040000\"]";
    }
    out << "]}}";
    return out.str();
}

struct WriterCloseGuard {
    BinaryTradeWriter& trade_writer;
    BinaryOrderBookWriter& book_writer;
    bool trade_open = false;
    bool book_open = false;

    ~WriterCloseGuard() {
        try {
            if (book_open) {
                book_writer.close();
            }
            if (trade_open) {
                trade_writer.close();
            }
        } catch (...) {
            // 析构兜底清理不能继续抛异常，避免异常路径触发 terminate。
        }
    }
};

}  // namespace

PipelineBenchmarkResult run_pipeline_benchmark(const Config& config,
                                                uint64_t messages,
                                                bool enable_write,
                                                uint64_t message_gap_us) {
    const auto max_gap = static_cast<uint64_t>(
        std::numeric_limits<std::chrono::microseconds::rep>::max());
    if (message_gap_us > max_gap) {
        throw std::invalid_argument("benchmark message_gap_us exceeds supported range");
    }

    PipelineBenchmarkResult result;

    Metrics metrics;
    ConnectionManager mgr(config, metrics);

    const std::string symbol = config.symbols.empty() ? "btcusdt" : config.symbols[0];
    if (!is_valid_symbol(symbol)) {
        throw std::invalid_argument(
            "benchmark symbol must be alphanumeric and <= 15 chars");
    }
    const auto trade_msg = make_trade_message(symbol);
    const auto book_msg = make_orderbook_message(symbol);
    const auto sym_lower = lower_symbol(symbol);

    uint64_t trade_callbacks = 0;
    uint64_t book_callbacks = 0;
    uint64_t validation_errors = 0;

    TradeRingBuffer trade_cache(config.ring_buffer_size);
    OrderBookRingBuffer book_cache(config.ring_buffer_size);
    BinaryTradeWriter trade_writer;
    BinaryOrderBookWriter book_writer;
    WriterCloseGuard writer_guard{trade_writer, book_writer, false, false};
    const auto bench_dir = std::filesystem::path(config.data_dir) / "bench_pipeline";
    if (enable_write) {
        if (!trade_writer.open(bench_dir.string(), config.ring_buffer_size)) {
            throw std::runtime_error("failed to open benchmark trade writer");
        }
        writer_guard.trade_open = true;
        if (!book_writer.open(bench_dir.string(), config.ring_buffer_size)) {
            throw std::runtime_error("failed to open benchmark orderbook writer");
        }
        writer_guard.book_open = true;
    }

    mgr.set_trade_callback([&](const Trade& trade) {
        ++trade_callbacks;
        if (!symbol_equals(trade.symbol, sizeof(trade.symbol), sym_lower)) {
            ++validation_errors;
        }
        if (enable_write) {
            trade_cache.push(trade);
            if (!trade_writer.write(trade)) {
                ++validation_errors;
            }
        }
    });
    mgr.set_orderbook_callback([&](const OrderBookSnapshot& book) {
        ++book_callbacks;
        if (!symbol_equals(book.symbol, sizeof(book.symbol), sym_lower)) {
            ++validation_errors;
        }
        if (enable_write) {
            book_cache.push(book);
            if (!book_writer.write(book)) {
                ++validation_errors;
            }
        }
    });

    // pin 不在这里调：现在统一交给 ConnectionManager::on_raw_message 第一次进入
    // 时做（live / bench 共享同一条路径）。这里第一次 process_raw_message 会
    // 触发 pin，pin 完才开始计时。这意味着 setup（writer.open）已经完成、
    // writer 后台线程已 spawn，affinity 不会传染。
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < messages; ++i) {
        mgr.process_raw_message((i % 2 == 0) ? trade_msg : book_msg);
        if (message_gap_us > 0 && i + 1 < messages) {
            std::this_thread::sleep_for(std::chrono::microseconds(message_gap_us));
        }
    }
    const auto processing_end = std::chrono::steady_clock::now();

    result.messages = messages;
    result.trades = trade_callbacks;
    result.orderbooks = book_callbacks;
    result.expected_trades = messages / 2 + messages % 2;
    result.expected_orderbooks = messages / 2;
    result.parse_errors = metrics.parse_errors.load(std::memory_order_relaxed);
    result.callback_errors = metrics.callback_errors.load(std::memory_order_relaxed);
    result.validation_errors = validation_errors;
    if (enable_write) {
        book_writer.close();
        writer_guard.book_open = false;
        trade_writer.close();
        writer_guard.trade_open = false;
        result.trade_written = trade_writer.records_written();
        result.trade_dropped = trade_writer.records_dropped();
        result.orderbook_written = book_writer.records_written();
        result.orderbook_dropped = book_writer.records_dropped();
        result.writer_error = trade_writer.has_error() || book_writer.has_error();
    }
    const auto end = std::chrono::steady_clock::now();
    const double processing_seconds =
        std::chrono::duration<double>(processing_end - start).count();
    const double total_seconds = std::chrono::duration<double>(end - start).count();
    // 一次性收齐 4 个直方图的快照（在写入线程已停的情况下，串行 reset 不会漏样本）。
    result.pipeline_lat   = metrics.pipeline_latency.snapshot_and_reset();
    result.json_parse_lat = metrics.json_parse_latency.snapshot_and_reset();
    result.biz_parse_lat  = metrics.biz_parse_latency.snapshot_and_reset();
    result.callback_lat   = metrics.callback_latency.snapshot_and_reset();
    result.processing_seconds = processing_seconds;
    result.total_seconds = total_seconds;
    result.processing_msgs_per_sec = processing_seconds > 0.0
        ? static_cast<double>(messages) / processing_seconds
        : 0.0;
    result.total_msgs_per_sec = total_seconds > 0.0
        ? static_cast<double>(messages) / total_seconds
        : 0.0;
    result.p50_us = result.pipeline_lat.p50_us;
    result.p95_us = result.pipeline_lat.p95_us;
    result.p99_us = result.pipeline_lat.p99_us;
    result.p999_us = result.pipeline_lat.p999_us;
    result.max_us = result.pipeline_lat.max_us;
    return result;
}

}  // namespace mds
