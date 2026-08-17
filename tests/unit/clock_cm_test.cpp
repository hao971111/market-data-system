#include "network/connection_manager.h"

#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

std::string trade_msg(const std::string& stream_sym, int64_t trade_id, int64_t t_ms) {
    nlohmann::json outer = {
        {"stream", stream_sym + "@trade"},
        {"data",
         {{"e", "trade"},
          {"E", t_ms},
          {"s", stream_sym},
          {"t", trade_id},
          {"p", "1.0"},
          {"q", "0.01"},
          {"T", t_ms},
          {"m", false}}},
    };
    return outer.dump();
}

}  // namespace

TEST(ClockOffsetConnection, PastExchangeTsRecordsExtLatency) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);
    mgr.set_trade_callback([](const mds::Trade&) {});

    mgr.process_raw_message(trade_msg("btcusdt", 1, 1672515782136LL));

    EXPECT_EQ(metrics.ext_latency_negative_drop.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(metrics.trade_latency.snapshot_and_reset().count, 1u);
}

TEST(ClockOffsetConnection, FutureExchangeTsCountsNegativeDrop) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);
    mgr.set_trade_callback([](const mds::Trade&) {});

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    mgr.process_raw_message(trade_msg("btcusdt", 1, now_ms + 30'000));

    EXPECT_EQ(metrics.ext_latency_negative_drop.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(metrics.trade_latency.snapshot_and_reset().count, 0u);
}
