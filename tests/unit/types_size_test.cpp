#include <mds/types.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>

TEST(TypesSize, TradeIs56BytesAnd8ByteAligned) {
    EXPECT_EQ(sizeof(mds::Trade), 56u);
    EXPECT_EQ(sizeof(mds::Trade) % 8, 0u);
}

TEST(TypesSize, OrderBookLevelIs16BytesAnd8ByteAligned) {
    EXPECT_EQ(sizeof(mds::OrderBookLevel), 16u);
    EXPECT_EQ(sizeof(mds::OrderBookLevel) % 8, 0u);
}

TEST(TypesSize, OrderBookSnapshotIs664BytesAnd8ByteAligned) {
    EXPECT_EQ(sizeof(mds::OrderBookSnapshot), 664u);
    EXPECT_EQ(sizeof(mds::OrderBookSnapshot) % 8, 0u);
}

TEST(TypesSetSymbol, TradeTruncatesLongSymbol) {
    mds::Trade trade{};
    const std::string long_sym(32, 'A');
    trade.set_symbol(long_sym);

    EXPECT_EQ(std::strlen(trade.symbol), 15u);
    EXPECT_EQ(trade.symbol[15], '\0');
    EXPECT_EQ(std::string(trade.symbol), std::string(15, 'A'));
}

TEST(TypesSetSymbol, OrderBookSnapshotTruncatesLongSymbol) {
    mds::OrderBookSnapshot book{};
    const std::string long_sym(32, 'B');
    book.set_symbol(long_sym);

    EXPECT_EQ(std::strlen(book.symbol), 15u);
    EXPECT_EQ(book.symbol[15], '\0');
    EXPECT_EQ(std::string(book.symbol), std::string(15, 'B'));
}

TEST(TypesSetSymbol, TradeKeepsShortSymbolIntact) {
    mds::Trade trade{};
    trade.set_symbol("BTCUSDT");
    EXPECT_STREQ(trade.symbol, "BTCUSDT");
}
