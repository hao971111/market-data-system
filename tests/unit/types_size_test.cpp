#include <mds/types.h>

#include <gtest/gtest.h>

TEST(TypesSize, TradeIs56Bytes) {
    EXPECT_EQ(sizeof(mds::Trade), 56u);
}

TEST(TypesSize, OrderBookSnapshotIs664Bytes) {
    EXPECT_EQ(sizeof(mds::OrderBookSnapshot), 664u);
}
