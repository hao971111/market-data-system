#pragma once

#include "../config/config.h"
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
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t max_us = 0;
};

PipelineBenchmarkResult run_pipeline_benchmark(const Config& config,
                                                uint64_t messages,
                                                bool enable_write,
                                                uint64_t message_gap_us);

}  // namespace mds
