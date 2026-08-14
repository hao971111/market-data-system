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

std::string depth_msg(const std::string& stream_sym, int64_t last_update_id) {
    nlohmann::json outer = {
        {"stream", stream_sym + "@depth20@100ms"},
        {"data",
         {{"lastUpdateId", last_update_id},
          {"bids", nlohmann::json::array({nlohmann::json::array({"100.0", "1.0"})})},
          {"asks", nlohmann::json::array({nlohmann::json::array({"101.0", "1.0"})})}}},
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

TEST(OrderBookSeqConnection, ForwardJumpAndEqualAccepted) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    std::vector<int64_t> ids;
    mgr.set_orderbook_callback(
        [&](const mds::OrderBookSnapshot& ob) { ids.push_back(ob.last_update_id); });

    mgr.process_raw_message(depth_msg("btcusdt", 10));
    mgr.process_raw_message(depth_msg("btcusdt", 15));  // 向前跳：快照正常
    mgr.process_raw_message(depth_msg("btcusdt", 15));  // 相等：盘口可能没变

    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 15);
    EXPECT_EQ(ids[2], 15);
    EXPECT_EQ(metrics.orderbooks_parsed.load(), 3u);
    EXPECT_EQ(metrics.orderbook_id_rollback_count.load(), 0u);
}

TEST(OrderBookSeqConnection, RollbackDropped) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    std::vector<int64_t> ids;
    mgr.set_orderbook_callback(
        [&](const mds::OrderBookSnapshot& ob) { ids.push_back(ob.last_update_id); });

    mgr.process_raw_message(depth_msg("btcusdt", 10));
    mgr.process_raw_message(depth_msg("btcusdt", 8));   // 回退：丢弃
    mgr.process_raw_message(depth_msg("btcusdt", 12));  // 继续向前

    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 12);
    EXPECT_EQ(metrics.orderbooks_parsed.load(), 2u);
    EXPECT_EQ(metrics.orderbook_id_rollback_count.load(), 1u);
}
