#include "network/connection_manager.h"
#include "parser/parser.h"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

std::string trade_combined(const std::string& stream_sym, const std::string& data_s) {
    nlohmann::json outer = {
        {"stream", stream_sym + "@trade"},
        {"data",
         {{"e", "trade"},
          {"E", 1672515782136LL},
          {"s", data_s},
          {"t", 1},
          {"p", "1.0"},
          {"q", "0.01"},
          {"T", 1672515782136LL},
          {"m", false}}},
    };
    return outer.dump();
}

std::string depth_combined(const std::string& stream_sym) {
    nlohmann::json levels = nlohmann::json::array();
    for (int i = 0; i < mds::ORDERBOOK_DEPTH; ++i) {
        levels.push_back({std::to_string(100.0 - i), "1.0"});
    }
    nlohmann::json outer = {
        {"stream", stream_sym + "@depth20@100ms"},
        {"data", {{"bids", levels}, {"asks", levels}}},
    };
    return outer.dump();
}

}  // namespace

TEST(SymbolCase, TradeAndOrderBookShareLowerSymbol) {
    const auto trade = mds::Parser::parse_trade(nlohmann::json{
        {"e", "trade"},
        {"E", 1},
        {"s", "BTCUSDT"},
        {"t", 1},
        {"p", "1.0"},
        {"q", "0.01"},
        {"T", 1},
        {"m", false},
    });
    const auto book = mds::Parser::parse_orderbook(
        nlohmann::json{
            {"bids", nlohmann::json::array({{"1.0", "1.0"}})},
            {"asks", nlohmann::json::array({{"1.1", "1.0"}})},
        },
        "btcusdt");

    ASSERT_TRUE(trade.has_value());
    ASSERT_TRUE(book.has_value());
    EXPECT_STREQ(trade->symbol, book->symbol);
    EXPECT_STREQ(trade->symbol, "btcusdt");
}

TEST(SymbolCase, UnsubscribedSymbolCountsParseError) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    int trade_cb = 0;
    mgr.set_trade_callback([&](const mds::Trade&) { ++trade_cb; });

    mgr.process_raw_message(trade_combined("ethusdt", "ETHUSDT"));

    EXPECT_EQ(trade_cb, 0);
    EXPECT_EQ(metrics.parse_errors.load(), 1u);
}

TEST(SymbolCase, SubscribedTradeNormalizesUpperDataSymbol) {
    mds::Config cfg;
    cfg.symbols = {"btcusdt"};
    mds::Metrics metrics;
    mds::ConnectionManager mgr(cfg, metrics);

    std::string got;
    mgr.set_trade_callback([&](const mds::Trade& t) { got = t.symbol; });
    mgr.set_orderbook_callback([&](const mds::OrderBookSnapshot& b) {
        // also exercise depth path
        if (got.empty()) {
            got = b.symbol;
        }
    });

    mgr.process_raw_message(trade_combined("btcusdt", "BTCUSDT"));
    EXPECT_EQ(got, "btcusdt");
    EXPECT_EQ(metrics.parse_errors.load(), 0u);

    got.clear();
    mgr.process_raw_message(depth_combined("btcusdt"));
    EXPECT_EQ(got, "btcusdt");
    EXPECT_EQ(metrics.parse_errors.load(), 0u);
}
