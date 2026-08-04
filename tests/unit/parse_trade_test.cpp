#include "parser/parser.h"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json valid_trade_json() {
    return nlohmann::json{
        {"e", "trade"},
        {"E", 1672515782136LL},
        {"s", "BTCUSDT"},
        {"t", 12345},
        {"p", "42000.5"},
        {"q", "0.01"},
        {"T", 1672515782136LL},
        {"m", true},
    };
}

}  // namespace

TEST(ParseTrade, ParsesValidMessage) {
    const auto trade = mds::Parser::parse_trade(valid_trade_json());
    ASSERT_TRUE(trade.has_value());
    EXPECT_STREQ(trade->symbol, "BTCUSDT");
    EXPECT_EQ(trade->trade_id, 12345);
    EXPECT_DOUBLE_EQ(trade->price, 42000.5);
    EXPECT_DOUBLE_EQ(trade->quantity, 0.01);
    EXPECT_EQ(trade->timestamp_us, 1672515782136LL * 1000);
    EXPECT_TRUE(trade->is_buyer_maker);
}

TEST(ParseTrade, ParsesValidMessageFromString) {
    const auto trade = mds::Parser::parse_trade(valid_trade_json().dump());
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->trade_id, 12345);
}

TEST(ParseTrade, RejectsMissingRequiredFields) {
    for (const char* field : {"s", "t", "p", "q", "T", "m"}) {
        auto j = valid_trade_json();
        j.erase(field);
        EXPECT_FALSE(mds::Parser::parse_trade(j).has_value())
            << "expected nullopt when missing field: " << field;
    }
}

TEST(ParseTrade, RejectsNonNumericPrice) {
    auto not_string = valid_trade_json();
    not_string["p"] = 42000.5;  // Binance 用字符串；数字类型应拒绝
    EXPECT_FALSE(mds::Parser::parse_trade(not_string).has_value());

    auto bad_text = valid_trade_json();
    bad_text["p"] = "not-a-number";
    EXPECT_FALSE(mds::Parser::parse_trade(bad_text).has_value());
}

TEST(ParseTrade, RejectsNegativeTimestamp) {
    auto j = valid_trade_json();
    j["T"] = -1;
    EXPECT_FALSE(mds::Parser::parse_trade(j).has_value());
}

TEST(ParseTrade, TruncatesSymbolLongerThan15) {
    auto j = valid_trade_json();
    j["s"] = std::string(32, 'X');

    const auto trade = mds::Parser::parse_trade(j);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(std::string(trade->symbol), std::string(15, 'X'));
    EXPECT_EQ(trade->symbol[15], '\0');
}
