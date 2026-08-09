#pragma once

#include "../config/config.h"
#include "../monitor/latency_histogram.h"
#include <cstdint>

namespace mds {

struct PipelineBenchmarkResult {
    uint64_t messages = 0;
    uint64_t trades = 0;
    uint64_t orderbooks = 0;
    uint64_t expected_trades = 0;
    uint64_t expected_orderbooks = 0;
    uint64_t parse_errors = 0;
    uint64_t callback_errors = 0;
    uint64_t validation_errors = 0;
    uint64_t trade_written = 0;
    uint64_t trade_dropped = 0;
    uint64_t orderbook_written = 0;
    uint64_t orderbook_dropped = 0;
    bool writer_error = false;
    double processing_seconds = 0.0;
    double total_seconds = 0.0;
    double processing_msgs_per_sec = 0.0;
    double total_msgs_per_sec = 0.0;
    // pipeline = on_raw_message 进入到函数退出（保留旧字段做总览/兼容）
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t p999_us = 0;
    uint64_t max_us = 0;
    // 分段 snapshot：pipeline 总 + json_parse / biz_parse / callback 三段
    // 三段之和 ≈ pipeline；bench 跟 live 用同一份口径，方便直接对比
    LatencyHistogram::Snapshot pipeline_lat;
    LatencyHistogram::Snapshot json_parse_lat;
    LatencyHistogram::Snapshot biz_parse_lat;
    LatencyHistogram::Snapshot callback_lat;
};

// pin 行为通过 config.pin_cpu 注入：>= 0 时 ConnectionManager::on_raw_message
// 第一次进入会把当前线程绑到该核（live / bench 共享同一条路径）。
// 这里不再单独传 pin_cpu 参数，避免 bench 走两条不同的 pin 路径。
PipelineBenchmarkResult run_pipeline_benchmark(const Config& config,
                                                uint64_t messages,
                                                bool enable_write,
                                                uint64_t message_gap_us);

}  // namespace mds
