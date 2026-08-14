#include "network/connection_manager.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

std::string trade_msg(const std::string& stream_sym, int64_t trade_id) {
    nlohmann::json outer = {
        {"stream", stream_sym + "@trade"},
        {"data",
         {{"e", "trade"},
          {"E", 1672515782136LL},
          {"s", stream_sym},
          {"t", trade_id},
          {"p", "1.0"},
          {"q", "0.01"},
          {"T", 1672515782136LL},
          {"m", false}}},
    };
    return outer.dump();
}

}  // namespace

TEST(TradeSeqConnection, ContinuousAccepted) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    std::vector<int64_t> ids;
    mgr.set_trade_callback([&](const mds::Trade& t) { ids.push_back(t.trade_id); });

    EXPECT_TRUE(mgr.can_trade("btcusdt"));
    mgr.process_raw_message(trade_msg("btcusdt", 10));
    mgr.process_raw_message(trade_msg("btcusdt", 11));
    mgr.process_raw_message(trade_msg("btcusdt", 12));

    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
    EXPECT_EQ(ids[2], 12);
    EXPECT_TRUE(mgr.can_trade("btcusdt"));
    EXPECT_EQ(metrics.trades_parsed.load(), 3u);
    EXPECT_EQ(metrics.gap_count.load(), 0u);
    EXPECT_EQ(metrics.duplicate_count.load(), 0u);
}

TEST(TradeSeqConnection, DuplicateDoesNotCallback) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    int cb = 0;
    mgr.set_trade_callback([&](const mds::Trade&) { ++cb; });

    mgr.process_raw_message(trade_msg("btcusdt", 10));
    mgr.process_raw_message(trade_msg("btcusdt", 10));
    mgr.process_raw_message(trade_msg("btcusdt", 9));

    EXPECT_EQ(cb, 1);
    EXPECT_EQ(metrics.trades_parsed.load(), 1u);
    EXPECT_EQ(metrics.duplicate_count.load(), 2u);
    EXPECT_TRUE(mgr.can_trade("btcusdt"));
}

TEST(TradeSeqConnection, GapHoldsAndCanTradeFalse) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    int cb = 0;
    mgr.set_trade_callback([&](const mds::Trade&) { ++cb; });

    mgr.process_raw_message(trade_msg("btcusdt", 1));
    mgr.process_raw_message(trade_msg("btcusdt", 5));  // missing 2..4
    mgr.process_raw_message(trade_msg("btcusdt", 6));  // Hold

    EXPECT_EQ(cb, 1);
    EXPECT_FALSE(mgr.can_trade("btcusdt"));
    EXPECT_EQ(metrics.gap_count.load(), 1u);
    EXPECT_EQ(metrics.missing_records.load(), 3u);
    EXPECT_EQ(metrics.recovering_symbols.load(), 1u);
    EXPECT_EQ(metrics.trades_parsed.load(), 1u);
}
