#include "parser/parser.h"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json make_levels(int count, double start_price, double qty) {
    nlohmann::json levels = nlohmann::json::array();
    for (int i = 0; i < count; ++i) {
        levels.push_back({
            std::to_string(start_price - i),
            std::to_string(qty + i),
        });
    }
    return levels;
}

nlohmann::json valid_orderbook_json(int depth = mds::ORDERBOOK_DEPTH) {
    return nlohmann::json{
        {"lastUpdateId", 123},
        {"bids", make_levels(depth, 100.0, 1.0)},
        {"asks", make_levels(depth, 101.0, 2.0)},
    };
}

}  // namespace

TEST(ParseOrderBook, ParsesValidMessage) {
    const auto book = mds::Parser::parse_orderbook(valid_orderbook_json(), "BTCUSDT");
    ASSERT_TRUE(book.has_value());
    EXPECT_STREQ(book->symbol, "btcusdt");  // 入参大写也会归一成小写
    EXPECT_GT(book->recv_ts_us, 0);
    EXPECT_EQ(book->last_update_id, 123);

    EXPECT_DOUBLE_EQ(book->bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(book->bids[0].quantity, 1.0);
    EXPECT_DOUBLE_EQ(book->bids[19].price, 81.0);
    EXPECT_DOUBLE_EQ(book->bids[19].quantity, 20.0);

    EXPECT_DOUBLE_EQ(book->asks[0].price, 101.0);
    EXPECT_DOUBLE_EQ(book->asks[0].quantity, 2.0);
    EXPECT_DOUBLE_EQ(book->asks[19].price, 82.0);
    EXPECT_DOUBLE_EQ(book->asks[19].quantity, 21.0);
}

TEST(ParseOrderBook, RejectsWhenBidsIsNotArray) {
    auto j = valid_orderbook_json();
    j["bids"] = "not-an-array";
    EXPECT_FALSE(mds::Parser::parse_orderbook(j, "BTCUSDT").has_value());
}

TEST(ParseOrderBook, RejectsWhenLevelPriceOrQtyNotString) {
    auto price_num = valid_orderbook_json();
    price_num["bids"][0] = nlohmann::json::array({100.0, "1.0"});
    EXPECT_FALSE(mds::Parser::parse_orderbook(price_num, "BTCUSDT").has_value());

    auto qty_num = valid_orderbook_json();
    qty_num["asks"][0] = nlohmann::json::array({"101.0", 2.0});
    EXPECT_FALSE(mds::Parser::parse_orderbook(qty_num, "BTCUSDT").has_value());
}

TEST(ParseOrderBook, AcceptsFewerThan20LevelsAndZeroFillsRest) {
    // 当前实现：不足 20 档仍成功，剩余档位填 0（不是直接拒绝）
    constexpr int kDepth = 3;
    const auto book = mds::Parser::parse_orderbook(valid_orderbook_json(kDepth), "ETHUSDT");
    ASSERT_TRUE(book.has_value());

    EXPECT_DOUBLE_EQ(book->bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(book->bids[2].price, 98.0);
    EXPECT_DOUBLE_EQ(book->bids[3].price, 0.0);
    EXPECT_DOUBLE_EQ(book->bids[3].quantity, 0.0);
    EXPECT_DOUBLE_EQ(book->bids[19].price, 0.0);

    EXPECT_DOUBLE_EQ(book->asks[0].price, 101.0);
    EXPECT_DOUBLE_EQ(book->asks[2].price, 99.0);
    EXPECT_DOUBLE_EQ(book->asks[19].price, 0.0);
    EXPECT_DOUBLE_EQ(book->asks[19].quantity, 0.0);
}

TEST(ParseOrderBook, RejectsMissingOrNonIntegerLastUpdateId) {
    auto missing = valid_orderbook_json();
    missing.erase("lastUpdateId");
    EXPECT_FALSE(mds::Parser::parse_orderbook(missing, "BTCUSDT").has_value());

    auto as_str = valid_orderbook_json();
    as_str["lastUpdateId"] = "123";
    EXPECT_FALSE(mds::Parser::parse_orderbook(as_str, "BTCUSDT").has_value());

    auto zero = valid_orderbook_json();
    zero["lastUpdateId"] = 0;
    EXPECT_FALSE(mds::Parser::parse_orderbook(zero, "BTCUSDT").has_value());
}
